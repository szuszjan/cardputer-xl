package com.cardputerxl.companion

import android.util.Base64

/**
 * Thin helpers matching the firmware's own command/response shape exactly
 * (see thecodeimtalkingabout.cpp's applyBleCommand()) - kept separate from
 * BleCompanionManager so the wire format is easy to compare line-by-line
 * against the firmware source when the two drift.
 */
object Protocol {
    private fun encode(text: String): String =
        Base64.encodeToString(text.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)

    private fun decode(b64: String): String =
        String(Base64.decode(b64, Base64.DEFAULT), Charsets.UTF_8)

    fun action(name: String) = "ACTION:$name"
    const val STATUS = "STATUS"
    const val GET_NOTES = "GET_NOTES"
    fun putNotes(text: String) = "PUT_NOTES:${encode(text)}"
    const val GET_CLAB = "GET_CLAB"
    fun putClab(source: String) = "PUT_CLAB:${encode(source)}"
    fun runClab(source: String) = "RUN_CLAB:${encode(source)}"

    /** "NOTES:<b64>" / "CLAB:<b64>" -> decoded text, or null if the prefix doesn't match. */
    fun payload(response: String?, prefix: String): String? {
        if (response == null || !response.startsWith(prefix)) return null
        return decode(response.removePrefix(prefix))
    }

    /** "STATUS:batt=1,uptime=2,..." -> a key/value map. */
    fun parseStatus(response: String?): Map<String, String> {
        if (response == null || !response.startsWith("STATUS:")) return emptyMap()
        return response.removePrefix("STATUS:")
            .split(",")
            .mapNotNull { pair ->
                val idx = pair.indexOf('=')
                if (idx < 0) null else pair.substring(0, idx) to pair.substring(idx + 1)
            }
            .toMap()
    }
}
