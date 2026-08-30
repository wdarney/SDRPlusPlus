package org.sdrpp.sdrpp

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.ArrayDeque
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

internal class ChannelBankGattServer(
    private val activity: MainActivity,
    private val requestHandler: (String) -> String,
    private val subscriptionChanged: (Boolean, Boolean) -> Unit
) {
    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000")
        val PROTOCOL_UUID: UUID = UUID.fromString("7d2f0001-8c4b-4d7a-9a61-8e3c4f2a1000")
        val COMMAND_UUID: UUID = UUID.fromString("7d2f0002-8c4b-4d7a-9a61-8e3c4f2a1000")
        val RESPONSE_UUID: UUID = UUID.fromString("7d2f0003-8c4b-4d7a-9a61-8e3c4f2a1000")
        val STATE_UUID: UUID = UUID.fromString("7d2f0004-8c4b-4d7a-9a61-8e3c4f2a1000")
        val AUDIO_UUID: UUID = UUID.fromString("7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val FRAME_VERSION: Byte = 1
        const val FLAG_FIRST = 1
        const val FLAG_LAST = 2
        const val HEADER_SIZE = 8
        const val MAX_REQUEST_BYTES = 1024 * 1024
        private const val TAG = "ChannelBankGatt"
    }

    private data class RequestKey(val address: String, val messageId: Int)
    private data class Assembly(var nextOffset: Int, val bytes: java.io.ByteArrayOutputStream)
    private data class Outgoing(
        val device: BluetoothDevice,
        val characteristic: BluetoothGattCharacteristic,
        val value: ByteArray,
        val confirm: Boolean
    )

    private val manager = activity.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter: BluetoothAdapter? get() = manager.adapter
    private var server: BluetoothGattServer? = null
    private var advertiseCallback: AdvertiseCallback? = null
    private val connected = ConcurrentHashMap<String, BluetoothDevice>()
    private val mtuByAddress = ConcurrentHashMap<String, Int>()
    private val assemblies = ConcurrentHashMap<RequestKey, Assembly>()
    private val responseByAddress = ConcurrentHashMap<String, ByteArray>()
    private val stateByAddress = ConcurrentHashMap<String, ByteArray>()
    private val stateSubscribers = ConcurrentHashMap.newKeySet<String>()
    private val audioSubscribers = ConcurrentHashMap.newKeySet<String>()
    private val outgoing = ArrayDeque<Outgoing>()
    private var sending = false
    private var audioSequence = 0

    private lateinit var protocol: BluetoothGattCharacteristic
    private lateinit var command: BluetoothGattCharacteristic
    private lateinit var response: BluetoothGattCharacteristic
    private lateinit var state: BluetoothGattCharacteristic
    private lateinit var audio: BluetoothGattCharacteristic

    fun start(): Boolean {
        if (server != null) return true
        val bluetoothAdapter = adapter ?: return false
        if (!bluetoothAdapter.isEnabled || !hasPermissions()) return false

        protocol = BluetoothGattCharacteristic(
            PROTOCOL_UUID, BluetoothGattCharacteristic.PROPERTY_READ,
            BluetoothGattCharacteristic.PERMISSION_READ
        )
        command = BluetoothGattCharacteristic(
            COMMAND_UUID,
            BluetoothGattCharacteristic.PROPERTY_WRITE or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
            BluetoothGattCharacteristic.PERMISSION_WRITE
        )
        response = notifyingCharacteristic(RESPONSE_UUID, indicate = true)
        state = notifyingCharacteristic(STATE_UUID, indicate = false)
        audio = notifyingCharacteristic(AUDIO_UUID, indicate = false)

        val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(protocol)
        service.addCharacteristic(command)
        service.addCharacteristic(response)
        service.addCharacteristic(state)
        service.addCharacteristic(audio)

        server = manager.openGattServer(activity, callback) ?: return false
        if (server?.addService(service) != true) {
            stop()
            return false
        }
        if (bluetoothAdapter.bluetoothLeAdvertiser == null) {
            Log.w(TAG, "BLE advertiser is not available on this device")
            return false
        }
        startAdvertising(bluetoothAdapter, attempt = 0)
        return true
    }

    fun stop() {
        try {
            advertiseCallback?.let { adapter?.bluetoothLeAdvertiser?.stopAdvertising(it) }
        } catch (_: SecurityException) {}
        advertiseCallback = null
        server?.close()
        server = null
        connected.clear()
        assemblies.clear()
        responseByAddress.clear()
        stateByAddress.clear()
        stateSubscribers.clear()
        audioSubscribers.clear()
        synchronized(outgoing) { outgoing.clear(); sending = false }
        subscriptionChanged(false, false)
    }

    fun notifyState(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        stateSubscribers.forEach { address ->
            val device = connected[address] ?: return@forEach
            stateByAddress[address] = payload
            enqueueFramed(device, state, messageId = 0, payload = payload, confirm = false)
        }
    }

    fun publishAudio(pcmS16Le: ByteArray) {
        if (pcmS16Le.isEmpty()) return
        val sequence = audioSequence++ and 0xffff
        audioSubscribers.forEach { address ->
            val device = connected[address] ?: return@forEach
            enqueueFramed(device, audio, sequence, pcmS16Le, confirm = false)
        }
    }

    private fun notifyingCharacteristic(uuid: UUID, indicate: Boolean): BluetoothGattCharacteristic {
        val property = if (indicate) BluetoothGattCharacteristic.PROPERTY_INDICATE
                       else BluetoothGattCharacteristic.PROPERTY_NOTIFY
        val characteristic = BluetoothGattCharacteristic(
            uuid, BluetoothGattCharacteristic.PROPERTY_READ or property,
            BluetoothGattCharacteristic.PERMISSION_READ
        )
        characteristic.addDescriptor(BluetoothGattDescriptor(
            CCCD_UUID, BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE
        ))
        return characteristic
    }

    private fun hasPermissions(): Boolean {
        if (Build.VERSION.SDK_INT < 31) return true
        return activity.checkSelfPermission("android.permission.BLUETOOTH_ADVERTISE") == PackageManager.PERMISSION_GRANTED &&
               activity.checkSelfPermission("android.permission.BLUETOOTH_CONNECT") == PackageManager.PERMISSION_GRANTED
    }

    private fun startAdvertising(bluetoothAdapter: BluetoothAdapter, attempt: Int) {
        val advertiser = bluetoothAdapter.bluetoothLeAdvertiser ?: return
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_BALANCED)
            .setConnectable(true)
            .setTimeout(0)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
            .build()
        val serviceUuid = ParcelUuid(SERVICE_UUID)
        val dataBuilder = AdvertiseData.Builder().setIncludeDeviceName(false)
        val scanResponseBuilder = AdvertiseData.Builder().setIncludeDeviceName(false)

        when (attempt) {
            0 -> dataBuilder.addServiceUuid(serviceUuid)
            1 -> scanResponseBuilder.addServiceUuid(serviceUuid).setIncludeDeviceName(true)
            else -> dataBuilder.setIncludeDeviceName(true)
        }

        advertiseCallback = object : AdvertiseCallback() {
            override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
                Log.i(TAG, "BLE advertising started, attempt=$attempt")
            }

            override fun onStartFailure(errorCode: Int) {
                Log.w(TAG, "BLE advertising failed, attempt=$attempt, error=$errorCode")
                advertiseCallback = null
                if (attempt < 2) startAdvertising(bluetoothAdapter, attempt + 1)
            }
        }

        try {
            advertiser.startAdvertising(settings, dataBuilder.build(), scanResponseBuilder.build(), advertiseCallback)
            Log.i(TAG, "Requested BLE advertising start, attempt=$attempt")
        } catch (se: SecurityException) {
            Log.w(TAG, "BLE advertising blocked by permission", se)
        } catch (iae: IllegalArgumentException) {
            Log.w(TAG, "BLE advertising data rejected, attempt=$attempt", iae)
            advertiseCallback = null
            if (attempt < 2) startAdvertising(bluetoothAdapter, attempt + 1)
        }
    }

    private fun protocolJson(): ByteArray =
        """{"protocol":"sdrpp.channel-bank.gatt","version":1,"encoding":"json-utf8","frameHeader":"u8 version,u8 flags,u16le messageId,u32le offset","maxRequestBytes":1048576,"audio":{"format":"pcm_s16le","rate":48000,"channels":1}}"""
            .toByteArray(Charsets.UTF_8)

    private fun sendRead(device: BluetoothDevice, requestId: Int, offset: Int, value: ByteArray) {
        if (offset < 0 || offset > value.size) {
            server?.sendResponse(device, requestId, BluetoothGatt.GATT_INVALID_OFFSET, offset, null)
            return
        }
        val mtu = mtuByAddress[device.address] ?: 23
        val end = minOf(value.size, offset + maxOf(1, mtu - 1))
        server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value.copyOfRange(offset, end))
    }

    private fun handleCommand(device: BluetoothDevice, value: ByteArray): Int {
        if (value.size < HEADER_SIZE) return BluetoothGatt.GATT_INVALID_ATTRIBUTE_LENGTH
        val header = ByteBuffer.wrap(value, 0, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        val version = header.get()
        val flags = header.get().toInt() and 0xff
        val messageId = header.short.toInt() and 0xffff
        val offset = header.int
        if (version != FRAME_VERSION || offset < 0) return BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED
        val key = RequestKey(device.address, messageId)
        val part = value.copyOfRange(HEADER_SIZE, value.size)
        val assembly = if ((flags and FLAG_FIRST) != 0) {
            Assembly(0, java.io.ByteArrayOutputStream()).also { assemblies[key] = it }
        } else assemblies[key] ?: return BluetoothGatt.GATT_INVALID_OFFSET
        if (offset != assembly.nextOffset || assembly.nextOffset + part.size > MAX_REQUEST_BYTES)
            return BluetoothGatt.GATT_INVALID_OFFSET
        assembly.bytes.write(part)
        assembly.nextOffset += part.size
        if ((flags and FLAG_LAST) != 0) {
            assemblies.remove(key)
            val request = assembly.bytes.toByteArray().toString(Charsets.UTF_8)
            val result = try { requestHandler(request) }
                         catch (t: Throwable) {
                             """{"v":1,"id":$messageId,"ok":false,"status":500,"error":{"code":"native_error","message":"${escape(t.message ?: "request failed")}"}}"""
                         }
            val responseBytes = result.toByteArray(Charsets.UTF_8)
            responseByAddress[device.address] = responseBytes
            enqueueFramed(device, response, messageId, responseBytes, confirm = true)
        }
        return BluetoothGatt.GATT_SUCCESS
    }

    private fun enqueueFramed(
        device: BluetoothDevice,
        characteristic: BluetoothGattCharacteristic,
        messageId: Int,
        payload: ByteArray,
        confirm: Boolean
    ) {
        val mtu = mtuByAddress[device.address] ?: 23
        val partSize = maxOf(1, mtu - 3 - HEADER_SIZE)
        val frameCount = maxOf(1, (payload.size + partSize - 1) / partSize)
        synchronized(outgoing) {
            // State and audio are lossy streams. Drop a complete message before
            // framing it so a client never receives an unterminated partial one.
            if (!confirm && outgoing.size + frameCount > 256) return
        }
        var offset = 0
        do {
            val count = minOf(partSize, payload.size - offset)
            var flags = 0
            if (offset == 0) flags = flags or FLAG_FIRST
            if (offset + count >= payload.size) flags = flags or FLAG_LAST
            val frame = ByteBuffer.allocate(HEADER_SIZE + count).order(ByteOrder.LITTLE_ENDIAN)
                .put(FRAME_VERSION).put(flags.toByte()).putShort(messageId.toShort()).putInt(offset)
            if (count > 0) frame.put(payload, offset, count)
            synchronized(outgoing) {
                outgoing.add(Outgoing(device, characteristic, frame.array(), confirm))
            }
            offset += count
        } while (offset < payload.size)
        sendNext()
    }

    private fun sendNext() {
        val item = synchronized(outgoing) {
            if (sending || outgoing.isEmpty()) return
            sending = true
            outgoing.first()
        }
        item.characteristic.value = item.value
        val accepted = try { server?.notifyCharacteristicChanged(item.device, item.characteristic, item.confirm) == true }
                       catch (_: SecurityException) { false }
        if (!accepted) {
            synchronized(outgoing) { if (outgoing.isNotEmpty()) outgoing.removeFirst(); sending = false }
            sendNext()
        }
    }

    private fun updateSubscriptions() {
        subscriptionChanged(stateSubscribers.isNotEmpty(), audioSubscribers.isNotEmpty())
    }

    private val callback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                connected[device.address] = device
                mtuByAddress[device.address] = 23
            } else {
                connected.remove(device.address)
                mtuByAddress.remove(device.address)
                stateSubscribers.remove(device.address)
                audioSubscribers.remove(device.address)
                assemblies.keys.removeAll { it.address == device.address }
                updateSubscriptions()
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
            mtuByAddress[device.address] = mtu.coerceAtLeast(23)
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int,
            characteristic: BluetoothGattCharacteristic
        ) {
            val value = when (characteristic.uuid) {
                PROTOCOL_UUID -> protocolJson()
                RESPONSE_UUID -> responseByAddress[device.address] ?: ByteArray(0)
                STATE_UUID -> requestHandler("""{"v":1,"id":0,"method":"GET","path":"/api/state"}""")
                    .toByteArray(Charsets.UTF_8).also { stateByAddress[device.address] = it }
                else -> ByteArray(0)
            }
            sendRead(device, requestId, offset, value)
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray
        ) {
            val status = if (characteristic.uuid != COMMAND_UUID) BluetoothGatt.GATT_WRITE_NOT_PERMITTED
                         else if (preparedWrite || offset != 0) BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED
                         else handleCommand(device, value)
            if (responseNeeded) server?.sendResponse(device, requestId, status, 0, null)
        }

        override fun onDescriptorReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int, descriptor: BluetoothGattDescriptor
        ) {
            val enabled = when (descriptor.characteristic.uuid) {
                STATE_UUID -> stateSubscribers.contains(device.address)
                AUDIO_UUID -> audioSubscribers.contains(device.address)
                RESPONSE_UUID -> true
                else -> false
            }
            sendRead(device, requestId, offset,
                if (enabled) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice, requestId: Int, descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray
        ) {
            var status = BluetoothGatt.GATT_SUCCESS
            if (descriptor.uuid != CCCD_UUID || preparedWrite || offset != 0) {
                status = BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED
            } else {
                val enabled = value.contentEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ||
                              value.contentEquals(BluetoothGattDescriptor.ENABLE_INDICATION_VALUE)
                when (descriptor.characteristic.uuid) {
                    STATE_UUID -> if (enabled) stateSubscribers.add(device.address) else stateSubscribers.remove(device.address)
                    AUDIO_UUID -> if (enabled) audioSubscribers.add(device.address) else audioSubscribers.remove(device.address)
                }
                updateSubscriptions()
            }
            if (responseNeeded) server?.sendResponse(device, requestId, status, 0, null)
        }

        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            synchronized(outgoing) {
                if (outgoing.isNotEmpty()) outgoing.removeFirst()
                sending = false
            }
            sendNext()
        }
    }

    private fun escape(value: String): String = value.replace("\\", "\\\\").replace("\"", "\\\"")
}
