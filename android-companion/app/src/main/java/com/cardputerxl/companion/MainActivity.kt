@file:OptIn(ExperimentalMaterial3Api::class)

package com.cardputerxl.companion

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

private enum class Screen(val label: String) {
    CONNECT("Connect"), REMOTE("Remote"), NOTES("Notes"), CLAB("C LAB")
}

class MainActivity : ComponentActivity() {

    private lateinit var ble: BleCompanionManager

    private val requiredPermissions: Array<String>
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private var permissionsGranted by mutableStateOf(false)

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result -> permissionsGranted = result.values.all { it } }

    private fun hasPermissions() = requiredPermissions.all {
        checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ble = BleCompanionManager(applicationContext)
        permissionsGranted = hasPermissions()

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    if (!permissionsGranted) {
                        PermissionGate(onRequest = { permissionLauncher.launch(requiredPermissions) })
                    } else {
                        CompanionApp(ble)
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        ble.disconnect()
    }
}

@Composable
private fun PermissionGate(onRequest: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text("Bluetooth permission needed", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(12.dp))
        Text(
            "Cardputer Companion needs Bluetooth access to scan for and connect to your Cardputer.",
            style = MaterialTheme.typography.bodyMedium
        )
        Spacer(Modifier.height(20.dp))
        Button(onClick = onRequest) { Text("Grant permission") }
    }
}

@Composable
private fun CompanionApp(ble: BleCompanionManager) {
    var screen by remember { mutableStateOf(Screen.CONNECT) }
    val connectionState by ble.state.collectAsState()

    Scaffold(
        bottomBar = {
            NavigationBar {
                Screen.entries.forEach { s ->
                    NavigationBarItem(
                        selected = screen == s,
                        onClick = { screen = s },
                        label = { Text(s.label) },
                        icon = {}
                    )
                }
            }
        },
        topBar = {
            TopAppBar(
                title = { Text("Cardputer Companion") },
                actions = {
                    val (dotColor, label) = when (connectionState) {
                        BleCompanionManager.ConnectionState.CONNECTED -> MaterialTheme.colorScheme.primary to "Connected"
                        BleCompanionManager.ConnectionState.CONNECTING,
                        BleCompanionManager.ConnectionState.SCANNING -> MaterialTheme.colorScheme.tertiary to "Connecting"
                        BleCompanionManager.ConnectionState.ERROR -> MaterialTheme.colorScheme.error to "Error"
                        else -> MaterialTheme.colorScheme.outline to "Offline"
                    }
                    AssistChip(onClick = {}, label = { Text(label) }, colors = AssistChipDefaults.assistChipColors(labelColor = dotColor))
                    Spacer(Modifier.width(12.dp))
                }
            )
        }
    ) { padding ->
        Box(modifier = Modifier.padding(padding).fillMaxSize()) {
            when (screen) {
                Screen.CONNECT -> ConnectScreen(ble)
                Screen.REMOTE -> RemoteScreen(ble)
                Screen.NOTES -> NotesScreen(ble)
                Screen.CLAB -> ClabScreen(ble)
            }
        }
    }
}

@Composable
private fun ConnectScreen(ble: BleCompanionManager) {
    var deviceName by remember { mutableStateOf("HijelHID KB") }
    val state by ble.state.collectAsState()
    val status by ble.statusLine.collectAsState()
    val scope = rememberCoroutineScope()
    var statusJson by remember { mutableStateOf<Map<String, String>>(emptyMap()) }

    Column(modifier = Modifier.fillMaxSize().padding(20.dp)) {
        Text("Connect to your Cardputer", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(4.dp))
        Text(
            "Open BLE COMPANION on the Cardputer (APPS list) and press ENTER to start advertising, then connect below.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(20.dp))
        OutlinedTextField(
            value = deviceName,
            onValueChange = { deviceName = it },
            label = { Text("BLE device name") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = { ble.scanAndConnect(deviceName) },
                enabled = state != BleCompanionManager.ConnectionState.SCANNING &&
                    state != BleCompanionManager.ConnectionState.CONNECTING
            ) { Text("Scan & Connect") }
            OutlinedButton(onClick = { ble.disconnect() }) { Text("Disconnect") }
        }
        Spacer(Modifier.height(20.dp))
        Text(status, style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.height(20.dp))
        if (state == BleCompanionManager.ConnectionState.CONNECTED) {
            Button(onClick = {
                scope.launch {
                    val response = ble.sendCommand(Protocol.STATUS)
                    statusJson = Protocol.parseStatus(response)
                }
            }) { Text("Refresh status") }
            Spacer(Modifier.height(12.dp))
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    listOf(
                        "Battery" to statusJson["batt"]?.let { "$it%" },
                        "Uptime" to statusJson["uptime"]?.let { "${it}s" },
                        "Wi-Fi" to statusJson["wifi"]?.let { if (it == "1") "connected" else "off" },
                        "Free heap" to statusJson["heap"]?.let { "${it}KB" },
                        "Current app" to statusJson["page"]
                    ).forEach { (label, value) ->
                        Row(modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
                            Text(label, modifier = Modifier.weight(1f), color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Text(value ?: "-")
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun RemoteScreen(ble: BleCompanionManager) {
    val state by ble.state.collectAsState()
    val connected = state == BleCompanionManager.ConnectionState.CONNECTED
    val scope = rememberCoroutineScope()
    fun send(action: String) = scope.launch { ble.sendCommand(Protocol.action(action)) }

    Column(
        modifier = Modifier.fillMaxSize().padding(20.dp).verticalScroll(rememberScrollState()),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        if (!connected) {
            Text(
                "Not connected - go to Connect first.",
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(bottom = 16.dp)
            )
        }
        Text("D-PAD", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        Row { Spacer(Modifier.width(72.dp)); RemoteButton("UP", connected) { send("up") } }
        Row {
            RemoteButton("LEFT", connected) { send("left") }
            RemoteButton("ENTER", connected) { send("enter") }
            RemoteButton("RIGHT", connected) { send("right") }
        }
        Row { Spacer(Modifier.width(72.dp)); RemoteButton("DOWN", connected) { send("down") } }
        Spacer(Modifier.height(8.dp))
        RemoteButton("BACK / HOME", connected, wide = true) { send("back") }

        Spacer(Modifier.height(24.dp))
        Text("QUICK APPS", style = MaterialTheme.typography.titleMedium)
        FlowRowSimple {
            RemoteChip("Dashboard", connected) { send("app-dashboard") }
            RemoteChip("Dice", connected) { send("app-dice") }
            RemoteChip("Notes", connected) { send("app-notes") }
            RemoteChip("Wi-Fi Scan", connected) { send("app-wifi") }
            RemoteChip("Music Lab", connected) { send("app-music") }
            RemoteChip("C LAB", connected) { send("app-clab") }
            RemoteChip("Settings", connected) { send("settings") }
        }

        Spacer(Modifier.height(24.dp))
        Text("QUICK SETTINGS", style = MaterialTheme.typography.titleMedium)
        FlowRowSimple {
            RemoteChip("Theme", connected) { send("theme") }
            RemoteChip("Bright -", connected) { send("bright-") }
            RemoteChip("Bright +", connected) { send("bright+") }
            RemoteChip("Volume -", connected) { send("volume-") }
            RemoteChip("Volume +", connected) { send("volume+") }
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun RemoteButton(label: String, enabled: Boolean, wide: Boolean = false, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = (if (wide) Modifier.fillMaxWidth(0.8f) else Modifier.size(width = 72.dp, height = 56.dp)).padding(4.dp)
    ) { Text(label, style = MaterialTheme.typography.labelSmall) }
}

@Composable
private fun RemoteChip(label: String, enabled: Boolean, onClick: () -> Unit) {
    AssistChip(
        onClick = onClick,
        label = { Text(label) },
        enabled = enabled,
        modifier = Modifier.padding(4.dp)
    )
}

/** A minimal wrapping row - avoids pulling in a separate layout dependency for this small a need. */
@Composable
private fun FlowRowSimple(content: @Composable () -> Unit) {
    Row(modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
        Column { content() }
    }
}

@Composable
private fun NotesScreen(ble: BleCompanionManager) {
    val state by ble.state.collectAsState()
    val connected = state == BleCompanionManager.ConnectionState.CONNECTED
    var text by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()

    Column(modifier = Modifier.fillMaxSize().padding(20.dp)) {
        Text("Notes (up to 15 lines on the Cardputer)", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            modifier = Modifier.fillMaxWidth().weight(1f),
            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace)
        )
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                enabled = connected,
                onClick = {
                    scope.launch {
                        status = "Loading..."
                        val response = ble.sendCommand(Protocol.GET_NOTES)
                        val payload = Protocol.payload(response, "NOTES:")
                        if (payload != null) { text = payload; status = "Loaded" } else status = "Failed to load"
                    }
                }
            ) { Text("Load from device") }
            Button(
                enabled = connected,
                onClick = {
                    scope.launch {
                        status = "Saving..."
                        val response = ble.sendCommand(Protocol.putNotes(text))
                        status = if (response == "OK") "Saved" else "Failed to save"
                    }
                }
            ) { Text("Save to device") }
        }
        Spacer(Modifier.height(8.dp))
        Text(status, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun ClabScreen(ble: BleCompanionManager) {
    val state by ble.state.collectAsState()
    val connected = state == BleCompanionManager.ConnectionState.CONNECTED
    var code by remember { mutableStateOf("") }
    var status by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()

    Column(modifier = Modifier.fillMaxSize().padding(20.dp)) {
        Text("C LAB source (32 lines / 48 chars max)", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(
            value = code,
            onValueChange = { code = it },
            modifier = Modifier.fillMaxWidth().weight(1f),
            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace)
        )
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                enabled = connected,
                onClick = {
                    scope.launch {
                        status = "Loading..."
                        val response = ble.sendCommand(Protocol.GET_CLAB)
                        val payload = Protocol.payload(response, "CLAB:")
                        if (payload != null) { code = payload; status = "Loaded" } else status = "Failed to load"
                    }
                }
            ) { Text("Load") }
            Button(
                enabled = connected,
                onClick = {
                    scope.launch {
                        status = "Saving..."
                        val response = ble.sendCommand(Protocol.putClab(code))
                        status = if (response == "OK") "Saved" else "Failed to save"
                    }
                }
            ) { Text("Save") }
            Button(
                enabled = connected,
                onClick = {
                    scope.launch {
                        status = "Running..."
                        val response = ble.sendCommand(Protocol.runClab(code))
                        status = if (response == "OK") "Saved & running on Cardputer" else "Failed to run"
                    }
                }
            ) { Text("Save & Run") }
        }
        Spacer(Modifier.height(8.dp))
        Text(status, style = MaterialTheme.typography.bodySmall)
    }
}
