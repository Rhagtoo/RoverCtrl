package org.rhanet.roverctrl.data

import android.content.Context

/**
 * Телеметрия (ровер → телефон, UDP :4211, JSON каждые 500мс)
 */
data class TelemetryData(
    val bat: Int = -1,
    val yaw: Float = 0f,
    val spd: Float = 0f,
    val pitch: Float = 0f,
    val roll: Float = 0f,
    val rssi: Int = 0,
    val rpmL: Float = Float.NaN,
    val rpmR: Float = Float.NaN
)

/**
 * Настройки подключения
 *
 * ИСПРАВЛЕНИЕ: Добавлено сохранение/загрузка из SharedPreferences
 *
 * НОВАЯ АРХИТЕКТУРА (AP mode):
 * - Ровер создаёт WiFi AP "RoverAP" с IP 192.168.4.1
 * - XIAO турель подключается к AP, получает IP 192.168.4.2
 * - Телефон подключается к AP
 */
data class ConnectionConfig(
    // Ровер — теперь создаёт AP, фиксированный IP
    val roverIp: String = "192.168.4.1",
    val cmdPort: Int = 4210,
    val telPort: Int = 4211,
    // XIAO турель — подключается к AP ровера
    val xiaoIp: String = "192.168.4.2",
    val xiaoPort: Int = 4210,
    val xiaoStreamPort: Int = 81
) {
    companion object {
        private const val PREFS = "rover_connection_config"

        // Ключи для SharedPreferences
        private const val KEY_ROVER_IP = "rover_ip"
        private const val KEY_CMD_PORT = "cmd_port"
        private const val KEY_TEL_PORT = "tel_port"
        private const val KEY_XIAO_IP = "xiao_ip"
        private const val KEY_XIAO_PORT = "xiao_port"
        private const val KEY_XIAO_STREAM_PORT = "xiao_stream_port"

        /**
         * Загрузить конфигурацию из SharedPreferences
         * Возвращает дефолтные значения если ничего не сохранено
         */
        fun load(ctx: Context): ConnectionConfig {
            val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

            return ConnectionConfig(
                roverIp = prefs.getString(KEY_ROVER_IP, "192.168.4.1") ?: "192.168.4.1",
                cmdPort = prefs.getInt(KEY_CMD_PORT, 4210),
                telPort = prefs.getInt(KEY_TEL_PORT, 4211),
                xiaoIp = prefs.getString(KEY_XIAO_IP, "192.168.4.2") ?: "192.168.4.2",
                xiaoPort = prefs.getInt(KEY_XIAO_PORT, 4210),
                xiaoStreamPort = prefs.getInt(KEY_XIAO_STREAM_PORT, 81)
            )
        }

        /**
         * Сохранить конфигурацию в SharedPreferences
         */
        fun save(ctx: Context, config: ConnectionConfig) {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString(KEY_ROVER_IP, config.roverIp)
                .putInt(KEY_CMD_PORT, config.cmdPort)
                .putInt(KEY_TEL_PORT, config.telPort)
                .putString(KEY_XIAO_IP, config.xiaoIp)
                .putInt(KEY_XIAO_PORT, config.xiaoPort)
                .putInt(KEY_XIAO_STREAM_PORT, config.xiaoStreamPort)
                .apply()
        }

        /**
         * Сбросить на дефолтные значения
         */
        fun reset(ctx: Context) {
            ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().clear().apply()
        }
    }

    /**
     * URL для MJPEG стрима с камеры турели
     */
    val turretStreamUrl: String
        get() = "http://$xiaoIp:$xiaoStreamPort/stream"

    /**
     * URL для одиночного кадра
     */
    val turretCaptureUrl: String
        get() = "http://$xiaoIp/capture"

    /**
     * URL для статуса турели
     */
    val turretStatusUrl: String
        get() = "http://$xiaoIp/status"

    /**
     * Валидация IP адреса
     */
    fun isValid(): Boolean {
        val ipRegex = Regex("""^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$""")
        return ipRegex.matches(roverIp) &&
               ipRegex.matches(xiaoIp) &&
               cmdPort in 1..65535 &&
               telPort in 1..65535 &&
               xiaoPort in 1..65535 &&
               xiaoStreamPort in 1..65535
    }
}

/**
 * Одометрия
 */
data class OdometryState(
    val x: Float = 0f,
    val y: Float = 0f,
    val headingRad: Float = 0f,
    val distM: Float = 0f
)

/**
 * Коробка передач
 */
object GearConfig {
    val MAX_SPEED = mapOf(1 to 50, 2 to 100)
}

/**
 * Режим трекинга
 */
enum class TrackingMode {
    MANUAL,       // джойстик рулит вручную
    LASER_DOT,    // HSV-маска красной точки → PID → Pan/Tilt
    OBJECT_TRACK, // YOLOv8 TFLite → PID → Pan/Tilt
    GYRO_TILT     // наклон телефона → Pan/Tilt (FPV-управление)
}

/**
 * Результат детекции (используется LaserTracker и ObjectTracker)
 */
data class DetectionResult(
    val cx: Float,
    val cy: Float,
    val w: Float = 0f,
    val h: Float = 0f,
    val confidence: Float,
    val label: String = ""
)
