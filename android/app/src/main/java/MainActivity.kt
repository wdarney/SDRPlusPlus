package org.sdrpp.sdrpp;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.hardware.usb.*;
import android.Manifest;
import android.os.Bundle;
import android.os.Build;
import android.view.View;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;
import android.util.Log;
import android.content.res.AssetManager;

import androidx.core.app.ActivityCompat;

import androidx.core.content.PermissionChecker;

import java.util.concurrent.LinkedBlockingQueue;
import java.io.*;

private const val ACTION_USB_PERMISSION = "org.sdrpp.sdrpp.USB_PERMISSION";

private fun requestDevicePermission(context: Context, dev: UsbDevice) {
    val activity = context as MainActivity
    val permissionIntent = PendingIntent.getBroadcast(activity, 0, Intent(ACTION_USB_PERMISSION), 0)
    activity.usbManager!!.requestPermission(dev, permissionIntent)
}

private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (ACTION_USB_PERMISSION == intent.action) {
            synchronized(this) {
                var _this = context as MainActivity;
                _this.SDR_device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    _this.SDR_conn = _this.usbManager!!.openDevice(_this.SDR_device);
                    
                    // Save SDR info
                    _this.SDR_VID = _this.SDR_device!!.getVendorId();
                    _this.SDR_PID = _this.SDR_device!!.getProductId()
                    _this.SDR_FD = _this.SDR_conn!!.getFileDescriptor();
                }
                
                // Hide again the system bars
                _this.hideSystemBars();
            }
        }
        else if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (dev != null) {
                requestDevicePermission(context, dev)
            }
        }
    }
}

class MainActivity : NativeActivity() {
    private val TAG : String = "SDR++";
    public var usbManager : UsbManager? = null;
    public var SDR_device : UsbDevice? = null;
    public var SDR_conn : UsbDeviceConnection? = null;
    public var SDR_VID : Int = -1;
    public var SDR_PID : Int = -1;
    public var SDR_FD : Int = -1;
    private var channelBankGatt: ChannelBankGattServer? = null
    private var channelBankGattWanted = false

    private external fun nativeChannelBankGattRequest(request: String): String
    private external fun nativeChannelBankGattSubscriptionChanged(state: Boolean, summary: Boolean, audio: Boolean)

    fun startChannelBankGatt() {
        channelBankGattWanted = true
        runOnUiThread {
            if (android.os.Build.VERSION.SDK_INT >= 31 &&
                (checkSelfPermission("android.permission.BLUETOOTH_ADVERTISE") != PackageManager.PERMISSION_GRANTED ||
                 checkSelfPermission("android.permission.BLUETOOTH_CONNECT") != PackageManager.PERMISSION_GRANTED)) {
                ActivityCompat.requestPermissions(this,
                    arrayOf("android.permission.BLUETOOTH_ADVERTISE", "android.permission.BLUETOOTH_CONNECT"), 42)
                return@runOnUiThread
            }
            if (channelBankGatt == null) {
                channelBankGatt = ChannelBankGattServer(
                    this,
                    { request -> nativeChannelBankGattRequest(request) },
                    { state, summary, audio -> nativeChannelBankGattSubscriptionChanged(state, summary, audio) }
                )
            }
            channelBankGatt?.start()
        }
    }

    fun stopChannelBankGatt() {
        channelBankGattWanted = false
        runOnUiThread {
            channelBankGatt?.stop()
            channelBankGatt = null
        }
    }

    fun notifyChannelBankGattState(json: String) {
        channelBankGatt?.notifyState(json)
    }

    fun notifyChannelBankGattSummary(json: String) {
        channelBankGatt?.notifySummary(json)
    }

    fun publishChannelBankGattAudio(pcmS16Le: ByteArray) {
        channelBankGatt?.publishAudio(pcmS16Le)
    }

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Hide bars
        hideSystemBars();

        // Ask for required permissions, without these the app cannot run.
        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE);

        // TODO: Have the main code wait until these two permissions are available

        // Register events
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager;
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        registerReceiver(usbReceiver, filter)

        // Get permission for all USB devices
        val devList = usbManager!!.getDeviceList();
        for ((name, dev) in devList) {
            requestDevicePermission(this, dev);
        }

        // Ask for internet permission
        checkAndAsk(Manifest.permission.INTERNET);

        super.onCreate(savedInstanceState)
    }

    public override fun onDestroy() {
        setKeepaliveActive(false);
        channelBankGatt?.stop()
        channelBankGatt = null
        super.onDestroy();
    }

    public override fun onResume() {
        // Hide bars again
        hideSystemBars();
        super.onResume();
        if (channelBankGattWanted) startChannelBankGatt()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 42 && channelBankGattWanted) startChannelBankGatt()
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.showSoftInput(window.decorView, 0);
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.hideSoftInputFromWindow(window.decorView.windowToken, 0);
        hideSystemBars();
    }

    fun setKeepaliveActive(active: Boolean) {
        val intent = Intent(this, SdrKeepaliveService::class.java);
        if (active) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent);
            }
            else {
                startService(intent);
            }
        }
        else {
            stopService(intent);
        }
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            unicodeCharacterQueue.offer(event.getUnicodeChar(event.metaState))
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc);
        var ecount = 0;
        for (fp in flist) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");

        return fdir;
    }
}
