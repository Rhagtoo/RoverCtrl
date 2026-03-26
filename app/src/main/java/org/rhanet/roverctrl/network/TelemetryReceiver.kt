package org.rhanet.roverctrl.network

import android.util.Log
import kotlinx.coroutines.*
import org.json.JSONObject
import org.rhanet.roverctrl.data.TelemetryData
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.SocketException

/**
 * TelemetryReceiver — приём телеметрии от ровера по UDP
 *
 * ИСПРАВЛЕНИЯ:
 * 1. Добавлен SO_REUSEADDR для переиспользования порта при быстром reconnect
 * 2. Добавлена обработка BindException с retry логикой
 * 3. Улучшена обработка таймаутов и ошибок
 *
 * Слушает UDP :port.
 * Ровер шлёт каждые 500мс:
 *   {"bat":N,"yaw":F,"spd":N,"pit":F,"rol":F,"rssi":N,"rpmL":F,"rpmR":F}
 */
class TelemetryReceiver {

    companion object {
        private const val TAG = "TelemetryRx"
        private const val MAX_BIND_RETRIES = 5
        private const val RETRY_DELAY_MS = 500L
        private const val SOCKET_TIMEOUT_MS = 1000
    }

    @Volatile
    private var running = false
    private var socket: DatagramSocket? = null
    private var lastPacketTime = 0L
    private var packetCount = 0L

    /**
     * Suspend-функция: слушает UDP в текущей корутине.
     * Отменяется через cancel() корутины.
     */
    suspend fun receive(
        port: Int,
        onData: (TelemetryData) -> Unit,
        onError: ((Throwable) -> Unit)? = null,
        onConnectionLost: (() -> Unit)? = null
    ) {
        withContext(Dispatchers.IO) {
            running = true
            var sock: DatagramSocket? = null

            // Пробуем привязаться к порту с retry
            for (attempt in 1..MAX_BIND_RETRIES) {
                try {
                    sock = DatagramSocket(null).apply {
                        reuseAddress = true  // ИСПРАВЛЕНИЕ: SO_REUSEADDR
                        bind(java.net.InetSocketAddress(port))
                        soTimeout = SOCKET_TIMEOUT_MS
                    }
                    Log.i(TAG, "Bound to port $port (attempt $attempt)")
                    break
                } catch (e: SocketException) {
                    Log.w(TAG, "Bind failed (attempt $attempt): ${e.message}")
                    sock?.close()
                    sock = null
                    if (attempt < MAX_BIND_RETRIES) {
                        delay(RETRY_DELAY_MS)
                    } else {
                        onError?.invoke(e)
                        return@withContext
                    }
                }
            }

            if (sock == null) {
                Log.e(TAG, "Failed to bind after $MAX_BIND_RETRIES attempts")
                return@withContext
            }

            socket = sock
            val buf = ByteArray(512)
            var consecutiveTimeouts = 0
            val maxConsecutiveTimeouts = 10 // 10 секунд без данных

            try {
                while (running && isActive) {
                    try {
                        val pkt = DatagramPacket(buf, buf.size)
                        sock.receive(pkt)

                        consecutiveTimeouts = 0
                        lastPacketTime = System.currentTimeMillis()
                        packetCount++

                        val json = JSONObject(String(buf, 0, pkt.length))
                        onData(parse(json))

                    } catch (_: java.net.SocketTimeoutException) {
                        // Нормально — проверяем running и ждём следующий пакет
                        consecutiveTimeouts++
                        if (consecutiveTimeouts >= maxConsecutiveTimeouts) {
                            Log.w(TAG, "Connection lost (no data for ${maxConsecutiveTimeouts}s)")
                            onConnectionLost?.invoke()
                            consecutiveTimeouts = 0
                        }
                    } catch (e: Exception) {
                        if (running) {
                            Log.w(TAG, "Receive error: ${e.message}")
                            onError?.invoke(e)
                        }
                    }
                }
            } finally {
                sock.close()
                socket = null
                Log.i(TAG, "Stopped. Total packets: $packetCount")
            }
        }
    }

    private fun parse(json: JSONObject): TelemetryData = TelemetryData(
        bat = json.optInt("bat", -1),
        yaw = json.optDouble("yaw", 0.0).toFloat(),
        spd = json.optDouble("spd", 0.0).toFloat(),
        pitch = json.optDouble("pit", 0.0).toFloat(),
        roll = json.optDouble("rol", 0.0).toFloat(),
        rssi = json.optInt("rssi", 0),
        rpmL = if (json.has("rpmL")) json.optDouble("rpmL").toFloat() else Float.NaN,
        rpmR = if (json.has("rpmR")) json.optDouble("rpmR").toFloat() else Float.NaN
    )

    fun stop() {
        running = false
        socket?.close()
    }

    /**
     * Время последнего полученного пакета (для диагностики)
     */
    fun getLastPacketTime(): Long = lastPacketTime

    /**
     * Количество принятых пакетов
     */
    fun getPacketCount(): Long = packetCount

    /**
     * Проверка активности соединения
     */
    fun isConnectionActive(timeoutMs: Long = 3000): Boolean {
        return lastPacketTime > 0 &&
               (System.currentTimeMillis() - lastPacketTime) < timeoutMs
    }
}
