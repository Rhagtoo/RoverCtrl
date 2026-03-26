package org.rhanet.roverctrl.network

import android.util.Log
import kotlinx.coroutines.*
import org.rhanet.roverctrl.data.ConnectionConfig
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * CommandSender — отправка команд роверу и турели по UDP
 *
 * ИСПРАВЛЕНИЯ:
 * 1. Добавлена явная синхронизация для DatagramSocket.send()
 * 2. Обновлены комментарии к протоколу (STR маппинг 40..140, не 145..35)
 * 3. Добавлена обработка ошибок отправки
 *
 * Протокол:
 *   Ровер  (по умолчанию 192.168.4.1) → "SPD:{};STR:{};FWD:{};LASER:{}\n"
 *     Прошивка: FWD: abs()→мощность, sign()→направление. SPD игнорируется.
 *     STR: -100..100 → map(-100,100, 40,140) → серво руля
 *
 *   Xiao турель (по умолчанию 192.168.4.2) → "PAN:{};TILT:{}\n"
 *     PAN:  -90..+90 → map(-90,90, 180,0) → серво (ИНВЕРТИРОВАН)
 *     TILT: -90..+90 → map(-90,90, 0,180) → серво (прямой)
 */
class CommandSender {

    companion object {
        private const val TAG = "CommandSender"
    }

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var socket: DatagramSocket? = null
    private val socketLock = ReentrantLock()  // ИСПРАВЛЕНИЕ: явная синхронизация

    private var roverAddr: InetAddress? = null
    private var roverPort: Int = 4210
    private var xiaoAddr: InetAddress? = null
    private var xiaoPort: Int = 4210

    private var lastSendTime = 0L
    private var sendCount = 0L
    private var errorCount = 0L

    init {
        try {
            socket = DatagramSocket()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create socket: ${e.message}")
        }
    }

    fun configure(cfg: ConnectionConfig) {
        try {
            roverAddr = InetAddress.getByName(cfg.roverIp)
            roverPort = cfg.cmdPort
            xiaoAddr = InetAddress.getByName(cfg.xiaoIp)
            xiaoPort = cfg.xiaoPort
            Log.i(TAG, "Configured: rover=${cfg.roverIp}:$roverPort, xiao=${cfg.xiaoIp}:$xiaoPort")
        } catch (e: Exception) {
            Log.e(TAG, "Configure failed: ${e.message}")
        }
    }

    fun clearHosts() {
        roverAddr = null
        xiaoAddr = null
    }

    /**
     * Единый метод отправки всех команд
     */
    fun send(spd: Int, str: Int, fwd: Int, laser: Boolean, pan: Int, tilt: Int, gear: Int = 2) {
        sendRover(spd, str, fwd, laser, gear)
        sendXiao(pan, tilt)
    }

    /**
     * Отправка команд ровера (движение + лазер + передача)
     */
    fun sendRover(spd: Int, str: Int, fwd: Int, laser: Boolean, gear: Int = 2) {
        val laserInt = if (laser) 1 else 0
        val msg = "SPD:$spd;STR:$str;FWD:$fwd;LASER:$laserInt;GEAR:$gear\n"
        scope.launch {
            roverAddr?.let { addr ->
                sendRaw(msg.toByteArray(), addr, roverPort)
            }
        }
    }

    /**
     * Отправка команд pan/tilt серво на Xiao турель.
     *
     * @param pan  -100..+100 (внутренний диапазон Android)
     * @param tilt -100..+100
     *
     * Масштабируем -100..100 → -90..90 перед отправкой.
     */
    fun sendXiao(pan: Int, tilt: Int) {
        // Масштабирование: Android -100..100 → UDP -90..90
        val panScaled = (pan * 0.9f).toInt().coerceIn(-90, 90)
        val tiltScaled = (tilt * 0.9f).toInt().coerceIn(-90, 90)

        val msg = "PAN:$panScaled;TILT:$tiltScaled\n"
        scope.launch {
            xiaoAddr?.let { addr ->
                sendRaw(msg.toByteArray(), addr, xiaoPort)
            }
        }
    }

    /**
     * Низкоуровневая отправка с синхронизацией
     */
    private fun sendRaw(data: ByteArray, addr: InetAddress, port: Int) {
        socketLock.withLock {  // ИСПРАВЛЕНИЕ: синхронизация доступа к сокету
            try {
                val sock = socket ?: return@withLock
                val pkt = DatagramPacket(data, data.size, addr, port)
                sock.send(pkt)
                lastSendTime = System.currentTimeMillis()
                sendCount++
            } catch (e: Exception) {
                errorCount++
                if (errorCount % 100 == 1L) {
                    Log.w(TAG, "Send error (total=$errorCount): ${e.message}")
                }
            }
        }
    }

    /**
     * Отправка экстренной остановки (вызывается при потере связи)
     */
    fun sendEmergencyStop() {
        scope.launch {
            roverAddr?.let { addr ->
                val msg = "SPD:0;STR:0;FWD:0;LASER:0\n"
                sendRaw(msg.toByteArray(), addr, roverPort)
            }
        }
    }

    fun close() {
        scope.cancel()
        socketLock.withLock {
            socket?.close()
            socket = null
        }
        Log.i(TAG, "Closed. Total sent: $sendCount, errors: $errorCount")
    }

    /**
     * Статистика для диагностики
     */
    fun getStats(): String {
        return "Sent: $sendCount, Errors: $errorCount, Last: ${System.currentTimeMillis() - lastSendTime}ms ago"
    }
}
