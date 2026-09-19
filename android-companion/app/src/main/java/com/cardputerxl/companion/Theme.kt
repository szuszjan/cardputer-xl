package com.cardputerxl.companion

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// The same cyan-on-dark "cyberdeck" identity CardputerXL's own firmware UI
// and browser ports already use (ui.accent, ui.bg in thecodeimtalkingabout.cpp)
// rather than borrowing a reference app's own palette - the useful part of
// looking at a well-made device-companion app is its structure (a device
// status card, stat tiles, an icon-grid control panel), not its colors.
private val CardputerCyan = Color(0xFF5AD1E6)
private val CardputerAmber = Color(0xFFF5A623)
private val CardputerGreen = Color(0xFF34D17C)
private val CardputerRed = Color(0xFFFF5B5B)
private val CardputerBg = Color(0xFF0B0D0F)
private val CardputerSurface = Color(0xFF13161A)
private val CardputerSurfaceVariant = Color(0xFF1B2024)
private val CardputerOutline = Color(0xFF2A3136)
private val CardputerText = Color(0xFFE9EDEE)
private val CardputerDim = Color(0xFF8A9296)

val CardputerColorScheme = darkColorScheme(
    primary = CardputerCyan,
    onPrimary = Color(0xFF00272E),
    secondary = CardputerAmber,
    onSecondary = Color(0xFF2B1900),
    tertiary = CardputerGreen,
    error = CardputerRed,
    background = CardputerBg,
    onBackground = CardputerText,
    surface = CardputerSurface,
    onSurface = CardputerText,
    surfaceVariant = CardputerSurfaceVariant,
    onSurfaceVariant = CardputerDim,
    outline = CardputerOutline,
)

val StatusOnline = CardputerGreen
val StatusConnecting = CardputerAmber
val StatusOffline = CardputerDim
val StatusError = CardputerRed

@Composable
fun CardputerCompanionTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = CardputerColorScheme, content = content)
}
