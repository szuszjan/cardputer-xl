@file:OptIn(ExperimentalMaterial3Api::class)

package com.cardputerxl.companion

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

private enum class Screen(val label: String) {
    DASHBOARD("Dashboard"), REMOTE("Remote"), NOTES("Notes"), CLAB("C LAB")
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
            CardputerCompanionTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
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

private fun statusColorAndLabel(state: BleCompanionManager.ConnectionState): Pair<Color, String> = when (state) {
    BleCompanionManager.ConnectionState.CONNECTED -> StatusOnline to "Connected"
    BleCompanionManager.ConnectionState.CONNECTING,
    BleCompanionManager.ConnectionState.SCANNING -> StatusConnecting to "Connecting"
    BleCompanionManager.ConnectionState.ERROR -> StatusError to "Error"
    else -> StatusOffline to "Offline"
}

@Composable
private fun CompanionApp(ble: BleCompanionManager) {
    var screen by remember { mutableStateOf(Screen.DASHBOARD) }
    val connectionState by ble.state.collectAsState()

    Scaffold(
        bottomBar = {
            NavigationBar {
                Screen.entries.forEach { s ->
                    NavigationBarItem(
                        selected = screen == s,
                        onClick = { screen = s },
                        label = { Text(s.label) },
                        icon = { NavGlyph(s, screen == s) }
                    )
                }
            }
        },
        topBar = {
            TopAppBar(
                title = { Text("Cardputer Companion", fontWeight = FontWeight.Bold) },
                actions = {
                    val (color, label) = statusColorAndLabel(connectionState)
                    StatusBadge(color, label)
                    Spacer(Modifier.width(12.dp))
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background)
            )
        },
        containerColor = MaterialTheme.colorScheme.background
    ) { padding ->
        Box(modifier = Modifier.padding(padding).fillMaxSize()) {
            when (screen) {
                Screen.DASHBOARD -> DashboardScreen(ble)
                Screen.REMOTE -> RemoteScreen(ble)
                Screen.NOTES -> NotesScreen(ble)
                Screen.CLAB -> ClabScreen(ble)
            }
        }
    }
}

@Composable
private fun NavGlyph(screen: Screen, selected: Boolean) {
    val glyph = when (screen) {
        Screen.DASHBOARD -> "▦"
        Screen.REMOTE -> "⬡"
        Screen.NOTES -> "≡"
        Screen.CLAB -> "{}"
    }
    Text(glyph, color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun StatusBadge(color: Color, label: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(modifier = Modifier.size(8.dp).clip(CircleShape).background(color))
        Spacer(Modifier.width(6.dp))
        Text(label, style = MaterialTheme.typography.labelMedium, color = color)
    }
}

// ---- DASHBOARD: a device card (glyph, name, status, primary action) plus
// a live-refreshing grid of stat tiles once connected - the same shape a
// good device-companion app's home screen takes (one card per device,
// tapping/opening it surfaces live status at a glance) rather than a bare
// text field and a button.
@Composable
private fun DashboardScreen(ble: BleCompanionManager) {
    var deviceName by remember { mutableStateOf("HijelHID KB") }
    val state by ble.state.collectAsState()
    val status by ble.statusLine.collectAsState()
    val scope = rememberCoroutineScope()
    var statusMap by remember { mutableStateOf<Map<String, String>>(emptyMap()) }
    val connected = state == BleCompanionManager.ConnectionState.CONNECTED

    // Keeps the stat tiles live while this screen is open and connected -
    // the same "just works, always current" feel a printer's own dashboard
    // has, rather than requiring a manual refresh every time.
    LaunchedEffect(connected) {
        while (connected) {
            val response = ble.sendCommand(Protocol.STATUS)
            statusMap = Protocol.parseStatus(response)
            delay(3000)
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState())
    ) {
        DeviceCard(deviceName, onNameChange = { deviceName = it }, state = state, status = status, onConnect = { ble.scanAndConnect(deviceName) }, onDisconnect = { ble.disconnect() })

        Spacer(Modifier.height(20.dp))
        Text("DEVICE STATUS", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))
        SimpleGrid(columns = 2) {
            item { StatTile("Battery", statusMap["batt"]?.let { "$it%" } ?: "--", MaterialTheme.colorScheme.primary) }
            item { StatTile("Uptime", statusMap["uptime"]?.let { formatUptime(it) } ?: "--", MaterialTheme.colorScheme.secondary) }
            item { StatTile("Wi-Fi", statusMap["wifi"]?.let { if (it == "1") "Online" else "Off" } ?: "--", StatusOnline) }
            item { StatTile("Free heap", statusMap["heap"]?.let { "${it}KB" } ?: "--", MaterialTheme.colorScheme.tertiary) }
        }
        Spacer(Modifier.height(12.dp))
        Card(
            modifier = Modifier.fillMaxWidth(),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
        ) {
            Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("CURRENT APP", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(statusMap["page"] ?: "--", style = MaterialTheme.typography.titleMedium)
                }
                TextButton(onClick = { scope.launch { statusMap = Protocol.parseStatus(ble.sendCommand(Protocol.STATUS)) } }, enabled = connected) {
                    Text("Refresh")
                }
            }
        }
    }
}

private fun formatUptime(seconds: String): String {
    val s = seconds.toLongOrNull() ?: return seconds
    val h = s / 3600; val m = (s % 3600) / 60
    return if (h > 0) "${h}h ${m}m" else "${m}m"
}

@Composable
private fun DeviceCard(
    deviceName: String,
    onNameChange: (String) -> Unit,
    state: BleCompanionManager.ConnectionState,
    status: String,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit
) {
    val (color, label) = statusColorAndLabel(state)
    val busy = state == BleCompanionManager.ConnectionState.SCANNING || state == BleCompanionManager.ConnectionState.CONNECTING
    var editing by remember { mutableStateOf(false) }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(Modifier.padding(20.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(52.dp)
                        .clip(RoundedCornerShape(14.dp))
                        .background(color.copy(alpha = 0.15f)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("▦", fontSize = 24.sp, color = color)
                }
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    if (editing) {
                        OutlinedTextField(
                            value = deviceName,
                            onValueChange = onNameChange,
                            singleLine = true,
                            label = { Text("BLE device name") },
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                            modifier = Modifier.fillMaxWidth()
                        )
                    } else {
                        Text(deviceName, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                        Spacer(Modifier.height(2.dp))
                        StatusBadge(color, label)
                    }
                }
                TextButton(onClick = { editing = !editing }) { Text(if (editing) "Done" else "Edit") }
            }
            Spacer(Modifier.height(14.dp))
            Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(14.dp))
            Button(
                onClick = { if (state == BleCompanionManager.ConnectionState.CONNECTED) onDisconnect() else onConnect() },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth(),
                colors = if (state == BleCompanionManager.ConnectionState.CONNECTED)
                    ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.surfaceVariant, contentColor = MaterialTheme.colorScheme.onSurface)
                else ButtonDefaults.buttonColors()
            ) {
                Text(
                    when {
                        busy -> "Connecting..."
                        state == BleCompanionManager.ConnectionState.CONNECTED -> "Disconnect"
                        else -> "Scan & Connect"
                    }
                )
            }
        }
    }
}

@Composable
private fun StatTile(label: String, value: String, accent: Color) {
    Card(
        modifier = Modifier.fillMaxWidth().height(84.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(Modifier.padding(14.dp).fillMaxSize(), verticalArrangement = Arrangement.SpaceBetween) {
            Box(modifier = Modifier.size(6.dp).clip(CircleShape).background(accent))
            Column {
                Text(value, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

/** A fixed-column grid built from Row/Column - avoids pulling in the separate lazy-grid foundation artifact for this small, non-scrolling a need. */
@Composable
private fun SimpleGrid(columns: Int, content: SimpleGridScope.() -> Unit) {
    val scope = SimpleGridScope().apply(content)
    val rows = scope.items.chunked(columns)
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        rows.forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { cell -> Box(Modifier.weight(1f)) { cell() } }
                repeat(columns - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }
    }
}

private class SimpleGridScope {
    val items = mutableListOf<@Composable () -> Unit>()
    fun item(content: @Composable () -> Unit) { items.add(content) }
}

// ---- REMOTE: a D-pad plus icon-tile grids for quick apps/settings -------
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
            Card(
                modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp),
                colors = CardDefaults.cardColors(containerColor = StatusError.copy(alpha = 0.12f))
            ) {
                Text(
                    "Not connected - go to Dashboard first.",
                    modifier = Modifier.padding(14.dp),
                    color = StatusError
                )
            }
        }
        Text("D-PAD", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(10.dp))
        Row { Spacer(Modifier.width(76.dp)); RemoteButton("UP", connected) { send("up") } }
        Row {
            RemoteButton("LEFT", connected) { send("left") }
            RemoteButton("ENTER", connected, accent = true) { send("enter") }
            RemoteButton("RIGHT", connected) { send("right") }
        }
        Row { Spacer(Modifier.width(76.dp)); RemoteButton("DOWN", connected) { send("down") } }
        Spacer(Modifier.height(10.dp))
        RemoteButton("BACK / HOME", connected, wide = true) { send("back") }

        Spacer(Modifier.height(28.dp))
        SectionLabel("QUICK APPS")
        SimpleGrid(columns = 3) {
            item { ControlTile("Dashboard", connected) { send("app-dashboard") } }
            item { ControlTile("Dice", connected) { send("app-dice") } }
            item { ControlTile("Notes", connected) { send("app-notes") } }
            item { ControlTile("Wi-Fi Scan", connected) { send("app-wifi") } }
            item { ControlTile("Music Lab", connected) { send("app-music") } }
            item { ControlTile("C LAB", connected) { send("app-clab") } }
            item { ControlTile("Settings", connected) { send("settings") } }
        }

        Spacer(Modifier.height(28.dp))
        SectionLabel("QUICK SETTINGS")
        SimpleGrid(columns = 3) {
            item { ControlTile("Theme", connected) { send("theme") } }
            item { ControlTile("Bright -", connected) { send("bright-") } }
            item { ControlTile("Bright +", connected) { send("bright+") } }
            item { ControlTile("Volume -", connected) { send("volume-") } }
            item { ControlTile("Volume +", connected) { send("volume+") } }
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun SectionLabel(text: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(bottom = 10.dp)) {
        Text(text, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun RemoteButton(label: String, enabled: Boolean, wide: Boolean = false, accent: Boolean = false, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = (if (wide) Modifier.fillMaxWidth(0.8f) else Modifier.size(width = 76.dp, height = 58.dp)).padding(4.dp),
        colors = if (accent) ButtonDefaults.buttonColors() else ButtonDefaults.buttonColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
            contentColor = MaterialTheme.colorScheme.onSurface
        )
    ) { Text(label, style = MaterialTheme.typography.labelSmall) }
}

@Composable
private fun ControlTile(label: String, enabled: Boolean, onClick: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .height(64.dp)
            .clickable(enabled = enabled, onClick = onClick),
        colors = CardDefaults.cardColors(
            containerColor = if (enabled) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.surface.copy(alpha = 0.5f)
        )
    ) {
        Box(Modifier.fillMaxSize().padding(8.dp), contentAlignment = Alignment.Center) {
            Text(
                label,
                style = MaterialTheme.typography.labelMedium,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
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
        Text("Notes", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Text("Up to 15 lines on the Cardputer", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            modifier = Modifier.fillMaxWidth().weight(1f),
            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace)
        )
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(
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
        Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
        Text("C LAB", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Text("32 lines / 48 chars max", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(
            value = code,
            onValueChange = { code = it },
            modifier = Modifier.fillMaxWidth().weight(1f),
            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace)
        )
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(
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
        Text(status, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
