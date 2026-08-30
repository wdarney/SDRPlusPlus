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
import org.json.JSONObject

internal class ChannelBankGattServer(
    private val activity: MainActivity,
    private val requestHandler: (String) -> String,
    private val subscriptionChanged: (Boolean, Boolean, Boolean) -> Unit
) {
    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000")
        val PROTOCOL_UUID: UUID = UUID.fromString("7d2f0001-8c4b-4d7a-9a61-8e3c4f2a1000")
        val COMMAND_UUID: UUID = UUID.fromString("7d2f0002-8c4b-4d7a-9a61-8e3c4f2a1000")
        val RESPONSE_UUID: UUID = UUID.fromString("7d2f0003-8c4b-4d7a-9a61-8e3c4f2a1000")
        val STATE_UUID: UUID = UUID.fromString("7d2f0004-8c4b-4d7a-9a61-8e3c4f2a1000")
        val AUDIO_UUID: UUID = UUID.fromString("7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000")
        val SUMMARY_UUID: UUID = UUID.fromString("7d2f0006-8c4b-4d7a-9a61-8e3c4f2a1000")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val FRAME_VERSION: Byte = 1
        const val FLAG_FIRST = 1
        const val FLAG_LAST = 2
        const val HEADER_SIZE = 8
        const val MAX_REQUEST_BYTES = 1024 * 1024
        const val MAX_ATTRIBUTE_VALUE_BYTES = 512
        private const val TAG = "ChannelBankGatt"
    }

    private data class RequestKey(val address: String, val messageId: Int)
    private data class Assembly(var nextOffset: Int, val bytes: java.io.ByteArrayOutputStream)
    private data class Outgoing(
        val device: BluetoothDevice,
        val characteristic: BluetoothGattCharacteristic,
        val value: ByteArray,
        val confirm: Boolean,
        val messageId: Int,
        val offset: Int,
        val flags: Int,
        val priority: Int
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
    private val summaryByAddress = ConcurrentHashMap<String, ByteArray>()
    private val stateSubscribers = ConcurrentHashMap.newKeySet<String>()
    private val summarySubscribers = ConcurrentHashMap.newKeySet<String>()
    private val audioSubscribers = ConcurrentHashMap.newKeySet<String>()
    private val responseOutgoing = ArrayDeque<Outgoing>()
    private val summaryOutgoing = ArrayDeque<Outgoing>()
    private val streamOutgoing = ArrayDeque<Outgoing>()
    private var sending: Outgoing? = null
    private var summaryMessageInProgress = false
    private var audioSequence = 0

    private lateinit var protocol: BluetoothGattCharacteristic
    private lateinit var command: BluetoothGattCharacteristic
    private lateinit var response: BluetoothGattCharacteristic
    private lateinit var state: BluetoothGattCharacteristic
    private lateinit var summary: BluetoothGattCharacteristic
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
        // State snapshots can span many ATT packets. Use acknowledged indications
        // so a dropped fragment cannot invalidate the rest of the JSON message.
        state = notifyingCharacteristic(STATE_UUID, indicate = true)
        summary = notifyingCharacteristic(SUMMARY_UUID, indicate = true)
        audio = notifyingCharacteristic(AUDIO_UUID, indicate = false)

        val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(protocol)
        service.addCharacteristic(command)
        service.addCharacteristic(response)
        service.addCharacteristic(state)
        service.addCharacteristic(summary)
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
        summaryByAddress.clear()
        stateSubscribers.clear()
        summarySubscribers.clear()
        audioSubscribers.clear()
        synchronized(streamOutgoing) {
            responseOutgoing.clear()
            summaryOutgoing.clear()
            streamOutgoing.clear()
            sending = null
            summaryMessageInProgress = false
        }
        subscriptionChanged(false, false, false)
    }

    fun notifyState(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        stateSubscribers.forEach { address ->
            val device = connected[address] ?: return@forEach
            stateByAddress[address] = payload
            enqueueFramed(device, state, messageId = 0, payload = payload, confirm = true)
        }
    }

    fun notifySummary(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        summarySubscribers.forEach { address ->
            val device = connected[address] ?: return@forEach
            summaryByAddress[address] = payload
            enqueueFramed(device, summary, messageId = 0, payload = payload, confirm = true)
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
        """{"protocol":"sdrpp.channel-bank.gatt","version":1,"encoding":"json-utf8","frameHeader":"u8 version,u8 flags,u16le messageId,u32le offset","maxAttributeValueBytes":512,"maxRequestBytes":1048576,"summaryCharacteristic":"7d2f0006-8c4b-4d7a-9a61-8e3c4f2a1000","audio":{"format":"pcm_s16le","rate":48000,"channels":1}}"""
            .toByteArray(Charsets.UTF_8)

    private fun sendRead(device: BluetoothDevice, requestId: Int, offset: Int, value: ByteArray) {
        if (offset < 0 || offset > value.size) {
            server?.sendResponse(device, requestId, BluetoothGatt.GATT_INVALID_OFFSET, offset, null)
            return
        }
        val mtu = mtuByAddress[device.address] ?: 23
        val end = minOf(value.size, offset + minOf(MAX_ATTRIBUTE_VALUE_BYTES, maxOf(1, mtu - 1)))
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
            try {
                val requestJson = JSONObject(request)
                val responseJson = JSONObject(result)
                val body = responseJson.optJSONObject("body")
                val runningValue = if (body?.has("running") == true) body.optBoolean("running") else null
                Log.i(TAG, "Command complete: id=$messageId method=${requestJson.optString("method")} " +
                    "path=${requestJson.optString("path")} requestBytes=${request.toByteArray(Charsets.UTF_8).size} " +
                    "responseBytes=${responseBytes.size} ok=${responseJson.optBoolean("ok")} running=$runningValue")
            } catch (_: Throwable) {}
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
        // ATT values are limited to 512 bytes even when the negotiated MTU is
        // 517 (whose MTU-3 notification budget would otherwise be 514).
        val frameSize = minOf(MAX_ATTRIBUTE_VALUE_BYTES, maxOf(HEADER_SIZE + 1, mtu - 3))
        val partSize = frameSize - HEADER_SIZE
        val priority = when (characteristic.uuid) {
            RESPONSE_UUID -> 0
            SUMMARY_UUID -> 1
            else -> 2
        }
        val frames = ArrayList<Outgoing>(maxOf(1, (payload.size + partSize - 1) / partSize))
        var offset = 0
        do {
            val count = minOf(partSize, payload.size - offset)
            var flags = 0
            if (offset == 0) flags = flags or FLAG_FIRST
            if (offset + count >= payload.size) flags = flags or FLAG_LAST
            val frame = ByteBuffer.allocate(HEADER_SIZE + count).order(ByteOrder.LITTLE_ENDIAN)
                .put(FRAME_VERSION).put(flags.toByte()).putShort(messageId.toShort()).putInt(offset)
            if (count > 0) frame.put(payload, offset, count)
            frames.add(Outgoing(device, characteristic, frame.array(), confirm,
                messageId, offset, flags, priority))
            offset += count
        } while (offset < payload.size)
        var outcome = "queued"
        var beforeResponses = 0
        var beforeSummaries = 0
        var beforeStreams = 0
        var afterResponses = 0
        var afterSummaries = 0
        var afterStreams = 0
        synchronized(streamOutgoing) {
            beforeResponses = responseOutgoing.size
            beforeSummaries = summaryOutgoing.size
            beforeStreams = streamOutgoing.size
            // Keep at most one complete State snapshot queued per client. The
            // next 500 ms publisher tick supplies the newest snapshot once it
            // drains, avoiding an unbounded reliable-indication backlog.
            if (characteristic.uuid == STATE_UUID && streamOutgoing.any {
                    it.device.address == device.address && it.characteristic.uuid == STATE_UUID
                }) {
                outcome = "coalesced"
            }
            else if (characteristic.uuid == SUMMARY_UUID && summaryOutgoing.any {
                    it.device.address == device.address
                }) {
                // Replace an entirely queued summary with the newest value.
                // Never remove the message currently awaiting its indication callback.
                val inFlight = sending
                summaryOutgoing.removeAll {
                    it.device.address == device.address && it !== inFlight
                }
                summaryOutgoing.addAll(frames)
                outcome = "replaced"
            }
            // Audio remains a lossy stream. Drop a complete message before it
            // enters the queue so a client never sees an unterminated partial.
            else if (!confirm && responseOutgoing.size + summaryOutgoing.size + streamOutgoing.size + frames.size > 256) {
                outcome = "skipped"
            }
            else if (priority == 0) responseOutgoing.addAll(frames)
            else if (priority == 1) summaryOutgoing.addAll(frames)
            else streamOutgoing.addAll(frames)
            afterResponses = responseOutgoing.size
            afterSummaries = summaryOutgoing.size
            afterStreams = streamOutgoing.size
        }
        if (characteristic.uuid == STATE_UUID) {
            Log.i(TAG, "State publish: timestamp=${System.currentTimeMillis()} payloadBytes=${payload.size} " +
                "fragments=${frames.size} mtu=$mtu outcome=$outcome " +
                "queueBefore=response:$beforeResponses,summary:$beforeSummaries,stream:$beforeStreams " +
                "queueAfter=response:$afterResponses,summary:$afterSummaries,stream:$afterStreams")
        } else if (characteristic.uuid == SUMMARY_UUID) {
            Log.i(TAG, "Summary publish: timestamp=${System.currentTimeMillis()} payloadBytes=${payload.size} " +
                "fragments=${frames.size} mtu=$mtu outcome=$outcome " +
                "queueBefore=response:$beforeResponses,summary:$beforeSummaries,stream:$beforeStreams " +
                "queueAfter=response:$afterResponses,summary:$afterSummaries,stream:$afterStreams")
        }
        if (outcome == "queued" || outcome == "replaced") sendNext()
    }

    private fun sendNext() {
        val item = synchronized(streamOutgoing) {
            if (sending != null) return
            val next = if (summaryMessageInProgress && summaryOutgoing.isNotEmpty()) summaryOutgoing.first()
                       else if (responseOutgoing.isNotEmpty()) responseOutgoing.first()
                       else if (summaryOutgoing.isNotEmpty()) summaryOutgoing.first()
                       else if (streamOutgoing.isNotEmpty()) streamOutgoing.first()
                       else return
            sending = next
            if (next.priority == 1 && (next.flags and FLAG_FIRST) != 0) {
                // Once a compact summary starts, finish it before a response
                // overtakes its remaining fragments. This lets clients apply
                // the newer summary sequence before an older command body.
                summaryMessageInProgress = true
            }
            next
        }
        item.characteristic.value = item.value
        val accepted = try { server?.notifyCharacteristicChanged(item.device, item.characteristic, item.confirm) == true }
                       catch (_: SecurityException) { false }
        if (!accepted) {
            synchronized(streamOutgoing) {
                val queue = when (item.priority) {
                    0 -> responseOutgoing
                    1 -> summaryOutgoing
                    else -> streamOutgoing
                }
                while (queue.isNotEmpty()) {
                    val dropped = queue.removeFirst()
                    if ((dropped.flags and FLAG_LAST) != 0) break
                }
                if (item.priority == 1) summaryMessageInProgress = false
                sending = null
            }
            Log.w(TAG, "Fragment rejected: characteristic=${item.characteristic.uuid} id=${item.messageId} " +
                "offset=${item.offset} flags=${item.flags}")
            sendNext()
        }
    }

    private fun updateSubscriptions() {
        subscriptionChanged(stateSubscribers.isNotEmpty(), summarySubscribers.isNotEmpty(), audioSubscribers.isNotEmpty())
    }

    private val callback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                connected[device.address] = device
                mtuByAddress[device.address] = 23
                Log.i(TAG, "Client connected, status=$status")
            } else {
                connected.remove(device.address)
                mtuByAddress.remove(device.address)
                stateSubscribers.remove(device.address)
                summarySubscribers.remove(device.address)
                audioSubscribers.remove(device.address)
                assemblies.keys.removeAll { it.address == device.address }
                synchronized(streamOutgoing) {
                    responseOutgoing.removeAll { it.device.address == device.address }
                    summaryOutgoing.removeAll { it.device.address == device.address }
                    streamOutgoing.removeAll { it.device.address == device.address }
                    if (sending?.device?.address == device.address) sending = null
                    if (sending == null) summaryMessageInProgress = false
                }
                updateSubscriptions()
                Log.i(TAG, "Client disconnected, status=$status")
                sendNext()
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
            mtuByAddress[device.address] = mtu.coerceAtLeast(23)
            Log.i(TAG, "Client MTU changed to $mtu")
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
                SUMMARY_UUID -> requestHandler("""{"v":1,"id":0,"method":"GET","path":"/api/state/summary"}""")
                    .toByteArray(Charsets.UTF_8).also { summaryByAddress[device.address] = it }
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
                SUMMARY_UUID -> summarySubscribers.contains(device.address)
                AUDIO_UUID -> audioSubscribers.contains(device.address)
                RESPONSE_UUID -> true
                else -> false
            }
            sendRead(device, requestId, offset,
                if (enabled && (descriptor.characteristic.uuid == STATE_UUID ||
                                descriptor.characteristic.uuid == SUMMARY_UUID ||
                                descriptor.characteristic.uuid == RESPONSE_UUID))
                    BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                else if (enabled) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
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
                    SUMMARY_UUID -> if (enabled) summarySubscribers.add(device.address) else summarySubscribers.remove(device.address)
                    AUDIO_UUID -> if (enabled) audioSubscribers.add(device.address) else audioSubscribers.remove(device.address)
                }
                updateSubscriptions()
                Log.i(TAG, "Subscriptions changed: state=${stateSubscribers.size}, summary=${summarySubscribers.size}, audio=${audioSubscribers.size}")
            }
            if (responseNeeded) server?.sendResponse(device, requestId, status, 0, null)
        }

        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.w(TAG, "Characteristic delivery failed, status=$status")
            }
            var delivered: Outgoing? = null
            var responseDepth = 0
            var summaryDepth = 0
            var streamDepth = 0
            synchronized(streamOutgoing) {
                val current = sending
                if (current == null || current.device.address != device.address) {
                    Log.w(TAG, "Unexpected delivery callback, status=$status")
                    return
                }
                delivered = current
                val queue = when (current.priority) {
                    0 -> responseOutgoing
                    1 -> summaryOutgoing
                    else -> streamOutgoing
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    if (queue.isNotEmpty()) queue.removeFirst()
                    if (current.priority == 1 && (current.flags and FLAG_LAST) != 0) {
                        summaryMessageInProgress = false
                    }
                } else {
                    // The failed fragment did not reach the client. Discard the
                    // remainder of that message instead of sending fragments
                    // that can only produce missing-first/offset errors.
                    while (queue.isNotEmpty()) {
                        val dropped = queue.removeFirst()
                        if (dropped.value.size >= HEADER_SIZE &&
                            (dropped.value[1].toInt() and FLAG_LAST) != 0) break
                    }
                    if (current.priority == 1) summaryMessageInProgress = false
                }
                sending = null
                responseDepth = responseOutgoing.size
                summaryDepth = summaryOutgoing.size
                streamDepth = streamOutgoing.size
            }
            delivered?.let {
                Log.d(TAG, "Fragment delivered: characteristic=${it.characteristic.uuid} id=${it.messageId} " +
                    "offset=${it.offset} first=${(it.flags and FLAG_FIRST) != 0} " +
                    "last=${(it.flags and FLAG_LAST) != 0} status=$status " +
                    "queueAfter=response:$responseDepth,summary:$summaryDepth,stream:$streamDepth")
            }
            sendNext()
        }
    }

    private fun escape(value: String): String = value.replace("\\", "\\\\").replace("\"", "\\\"")
}
