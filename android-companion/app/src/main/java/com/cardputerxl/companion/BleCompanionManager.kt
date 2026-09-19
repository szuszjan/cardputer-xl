package com.cardputerxl.companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID
import kotlin.math.min

/**
 * Talks to the Cardputer's "BLE COMPANION" app over its Nordic UART Service
 * (see thecodeimtalkingabout.cpp's own BLE COMPANION section) - a plain
 * newline-delimited text protocol: write a command to RX, read the
 * response back from TX notifications. Multi-line payloads (Notes, C LAB
 * source) travel base64-wrapped on both ends, matching the firmware's own
 * base64Encode()/base64Decode() convention, so an embedded newline in the
 * payload is never mistaken for the end of a message.
 *
 * Every BLE call here is one-at-a-time by design: a single in-flight
 * command (`pendingResponse`) and a single in-flight write chunk
 * (`writeContinuation`) at once, matching the firmware's own "one pending
 * command" buffering (pendingBleCommand) - there is no benefit to pipelining
 * requests the other side can't process concurrently anyway.
 */
class BleCompanionManager(private val context: Context) {

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        // Requested MTU - actual negotiated value may be lower, but the
        // firmware's own outgoing chunk size (20 bytes) already assumes the
        // worst case, so this is purely a throughput nicety, not a
        // correctness requirement.
        private const val REQUESTED_MTU = 247
        // Conservative outgoing write chunk - safely under even an
        // unnegotiated 23-byte ATT MTU's 20-byte usable payload.
        private const val WRITE_CHUNK = 20
        private const val COMMAND_TIMEOUT_MS = 6000L
        private const val WRITE_TIMEOUT_MS = 3000L
    }

    enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED, ERROR }

    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter get() = bluetoothManager.adapter

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null

    private val _state = MutableStateFlow(ConnectionState.DISCONNECTED)
    val state: StateFlow<ConnectionState> = _state
    private val _statusLine = MutableStateFlow("")
    val statusLine: StateFlow<String> = _statusLine

    private val lineBuffer = StringBuilder()
    private var pendingResponse: CompletableDeferred<String>? = null
    private var writeContinuation: CompletableDeferred<Boolean>? = null

    @SuppressLint("MissingPermission")
    fun scanAndConnect(deviceName: String) {
        disconnect()
        _statusLine.value = "Scanning for \"$deviceName\"..."
        _state.value = ConnectionState.SCANNING
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            _statusLine.value = "Bluetooth is off or unavailable"
            _state.value = ConnectionState.ERROR
            return
        }
        val filters = listOf(ScanFilter.Builder().setDeviceName(deviceName).build())
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(filters, settings, scanCallback)
    }

    @SuppressLint("MissingPermission")
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            adapter?.bluetoothLeScanner?.stopScan(this)
            _statusLine.value = "Found ${result.device.name ?: result.device.address} - connecting..."
            _state.value = ConnectionState.CONNECTING
            gatt = result.device.connectGatt(context, false, gattCallback)
        }

        override fun onScanFailed(errorCode: Int) {
            _statusLine.value = "Scan failed (code $errorCode)"
            _state.value = ConnectionState.ERROR
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _statusLine.value = "Connected - negotiating MTU..."
                    g.requestMtu(REQUESTED_MTU)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _statusLine.value = "Disconnected"
                    _state.value = ConnectionState.DISCONNECTED
                    rxChar = null; txChar = null
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                _statusLine.value = "Cardputer found, but BLE COMPANION isn't advertising its service - open BLE COMPANION on the device and press ENTER"
                _state.value = ConnectionState.ERROR
                return
            }
            rxChar = service.getCharacteristic(RX_UUID)
            txChar = service.getCharacteristic(TX_UUID)
            val tx = txChar
            if (tx == null || rxChar == null) {
                _statusLine.value = "BLE COMPANION service is missing a characteristic"
                _state.value = ConnectionState.ERROR
                return
            }
            g.setCharacteristicNotification(tx, true)
            val cccd = tx.getDescriptor(CCCD_UUID)
            if (cccd != null) {
                @Suppress("DEPRECATION")
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
            _statusLine.value = "Connected"
            _state.value = ConnectionState.CONNECTED
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleIncoming(value)
        }

        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (Build.VERSION.SDK_INT < 33) handleIncoming(characteristic.value ?: return)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            writeContinuation?.complete(status == BluetoothGatt.GATT_SUCCESS)
        }
    }

    private fun handleIncoming(bytes: ByteArray) {
        lineBuffer.append(String(bytes, Charsets.US_ASCII))
        var idx = lineBuffer.indexOf("\n")
        while (idx >= 0) {
            val line = lineBuffer.substring(0, idx)
            lineBuffer.delete(0, idx + 1)
            val waiter = pendingResponse
            if (waiter != null) {
                pendingResponse = null
                waiter.complete(line)
            }
            idx = lineBuffer.indexOf("\n")
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun writeChunk(bytes: ByteArray): Boolean {
        val ch = rxChar ?: return false
        val g = gatt ?: return false
        val deferred = CompletableDeferred<Boolean>()
        writeContinuation = deferred
        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            ch.value = bytes
            @Suppress("DEPRECATION")
            g.writeCharacteristic(ch)
        }
        if (!started) return false
        return withTimeoutOrNull(WRITE_TIMEOUT_MS) { deferred.await() } ?: false
    }

    /**
     * Sends one command line and waits for the matching response line.
     * Returns null on timeout or if not connected. Only one command may be
     * in flight at a time - callers on a busy UI should disable their own
     * controls while awaiting a result rather than queuing here.
     */
    suspend fun sendCommand(command: String): String? {
        if (_state.value != ConnectionState.CONNECTED) return null
        val full = (command + "\n").toByteArray(Charsets.US_ASCII)
        val deferred = CompletableDeferred<String>()
        pendingResponse = deferred
        var offset = 0
        while (offset < full.size) {
            val end = min(full.size, offset + WRITE_CHUNK)
            if (!writeChunk(full.copyOfRange(offset, end))) {
                pendingResponse = null
                return null
            }
            offset = end
        }
        return withTimeoutOrNull(COMMAND_TIMEOUT_MS) { deferred.await() }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxChar = null; txChar = null
        lineBuffer.clear()
        pendingResponse = null
        _state.value = ConnectionState.DISCONNECTED
    }
}
