package org.rhanet.roverctrl.ui

import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.rhanet.roverctrl.data.*
import org.rhanet.roverctrl.network.CommandSender
import org.rhanet.roverctrl.network.MjpegDecoder
import org.rhanet.roverctrl.network.TelemetryReceiver
import org.rhanet.roverctrl.tracking.CalibrationResult
import org.rhanet.roverctrl.tracking.OdometryTracker
import org.rhanet.roverctrl.tracking.PidController

/**
 * RoverViewModel — Shared ViewModel
 *
 * Управляет сетевыми подключениями, 20Hz тиком команд, одометрией,
 * MJPEG стримом с турели (XIAO Sense), режимами трекинга.
 *
 * ИСПРАВЛЕНИЯ:
 * 1. CalibrationResult — теперь использует существующий класс
 * 2. Bitmap утечка — исправлена в обработке MJPEG кадров
 * 3. Добавлена обработка потери соединения
 */
class RoverViewModel : ViewModel() {

    companion object {
        private const val TAG = "RoverVM"
        private const val CMD_TICK_MS = 50L  // 20Hz
    }

    // ── Сеть ──────────────────────────────────────────────────────────────
    val sender = CommandSender()
    val telemRx = TelemetryReceiver()

    // ── Состояния (UI наблюдает через StateFlow) ──────────────────────────
    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> get() = _connected

    private val _telem = MutableStateFlow(TelemetryData())
    val telem: StateFlow<TelemetryData> get() = _telem

    private val _trackMode = MutableStateFlow(TrackingMode.MANUAL)
    val trackMode: StateFlow<TrackingMode> get() = _trackMode

    private val _pose = MutableStateFlow(OdometryTracker.Pose(0f, 0f, 0f))
    val pose: StateFlow<OdometryTracker.Pose> get() = _pose

    private val _cameraFps = MutableStateFlow(0f)
    val cameraFps: StateFlow<Float> get() = _cameraFps

    // ── Калибровка (ИСПРАВЛЕНО: используем CalibrationResult) ───────────
    private val _calibration = MutableStateFlow(CalibrationResult())
    val calibration: StateFlow<CalibrationResult> get() = _calibration

    // ── MJPEG стрим с камеры турели (XIAO Sense) ────────────────────────
    private val _turretFrame = MutableStateFlow<Bitmap?>(null)
    val turretFrame: StateFlow<Bitmap?> get() = _turretFrame

    private val _turretFps = MutableStateFlow(0f)
    val turretFps: StateFlow<Float> get() = _turretFps

    private val _turretConnected = MutableStateFlow(false)
    val turretConnected: StateFlow<Boolean> get() = _turretConnected

    private var mjpegDecoder: MjpegDecoder? = null

    // ── Одометрия ─────────────────────────────────────────────────────────
    private val odometry = OdometryTracker()

    // ── PID контроллеры для pan/tilt ──────────────────────────────────────
    val pidPan = PidController(kp = 120f, ki = 0.5f, kd = 8f, outMax = 100f)
    val pidTilt = PidController(kp = 120f, ki = 0.5f, kd = 8f, outMax = 100f)

    // ── Передача ──────────────────────────────────────────────────────────
    private val _gear = MutableStateFlow(2)
    val gear: StateFlow<Int> get() = _gear

    fun setGear(g: Int) { _gear.value = g }

    // ── Команды ───────────────────────────────────────────────────────────
    private var spd = 0
    private var str = 0
    private var fwd = 0
    private var panCmd = 0
    private var tiltCmd = 0
    var laserOn = false

    // ── Coroutine jobs ────────────────────────────────────────────────────
    private var cmdTickJob: Job? = null
    private var telemJob: Job? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    // ── Конфигурация ──────────────────────────────────────────────────────
    private var currentConfig: ConnectionConfig? = null

    // ══════════════════════════════════════════════════════════════════════
    // Подключение
    // ══════════════════════════════════════════════════════════════════════

    fun connect(cfg: ConnectionConfig) {
        if (_connected.value) return

        currentConfig = cfg
        sender.configure(cfg)

        // Запуск приёма телеметрии
        telemJob = viewModelScope.launch {
            telemRx.receive(
                port = cfg.telPort,
                onData = { data ->
                    _telem.value = data
                    updateOdometry(data)
                },
                onError = { e ->
                    Log.w(TAG, "Telemetry error: ${e.message}")
                },
                onConnectionLost = {
                    Log.w(TAG, "Telemetry connection lost")
                    mainHandler.post {
                        // Можно уведомить UI
                    }
                }
            )
        }

        // Запуск 20Hz тика команд
        cmdTickJob = viewModelScope.launch {
            while (true) {
                sender.send(spd, str, fwd, laserOn, panCmd, tiltCmd, _gear.value)
                delay(CMD_TICK_MS)
            }
        }

        // Запуск MJPEG стрима с турели
        startTurretStream(cfg.turretStreamUrl)

        _connected.value = true
        Log.i(TAG, "Connected to rover=${cfg.roverIp}, xiao=${cfg.xiaoIp}")
    }

    fun disconnect() {
        cmdTickJob?.cancel()
        telemJob?.cancel()
        telemRx.stop()
        sender.clearHosts()
        stopTurretStream()

        // Сброс состояния
        spd = 0; str = 0; fwd = 0
        panCmd = 0; tiltCmd = 0
        laserOn = false
        _telem.value = TelemetryData()

        _connected.value = false
        Log.i(TAG, "Disconnected")
    }

    // ══════════════════════════════════════════════════════════════════════
    // Одометрия
    // ══════════════════════════════════════════════════════════════════════

    private fun updateOdometry(data: TelemetryData) {
        odometry.update(
            rpmL = data.rpmL,
            rpmR = data.rpmR,
            spdPct = data.spd,
            strPct = str.toFloat()
        )
        _pose.value = odometry.pose
    }

    // ══════════════════════════════════════════════════════════════════════
    // MJPEG турель (ИСПРАВЛЕНО: bitmap утечка)
    // ══════════════════════════════════════════════════════════════════════

    private fun startTurretStream(url: String) {
        stopTurretStream()
        Log.i(TAG, "Starting turret MJPEG stream: $url")

        mjpegDecoder = MjpegDecoder(
            url = url,
            onFrame = { bmp ->
                // ИСПРАВЛЕНИЕ: Создаём копию и сразу освобождаем оригинал в UI потоке
                val copy = bmp.copy(Bitmap.Config.ARGB_8888, false)
                
                mainHandler.post {
                    // Освобождаем предыдущий кадр
                    _turretFrame.value?.recycle()
                    _turretFrame.value = copy
                    _turretConnected.value = true
                }
                
                // ИСПРАВЛЕНИЕ: Освобождаем оригинальный bitmap после копирования
                bmp.recycle()
            },
            onFps = { fps ->
                mainHandler.post { _turretFps.value = fps }
            },
            onError = { e ->
                Log.w(TAG, "MJPEG error: ${e.message}")
                mainHandler.post { _turretConnected.value = false }
            }
        ).also { it.start() }
    }

    private fun stopTurretStream() {
        mjpegDecoder?.halt()
        mjpegDecoder = null
        _turretFrame.value?.recycle()
        _turretFrame.value = null
        _turretFps.value = 0f
        _turretConnected.value = false
    }

    // ══════════════════════════════════════════════════════════════════════
    // Управление
    // ══════════════════════════════════════════════════════════════════════

    /** Вызывается из ControlFragment каждые 50 мс */
    fun setDriveCmd(spd: Int, str: Int, fwd: Int) {
        val maxSpd = GearConfig.MAX_SPEED[_gear.value] ?: 100
        this.spd = spd
        this.str = str
        this.fwd = fwd.coerceIn(-maxSpd, maxSpd)
    }

    /** Вызывается из VideoFragment при трекинге или из ControlFragment */
    fun setPanTilt(pan: Int, tilt: Int) {
        panCmd = pan
        tiltCmd = tilt
    }

    fun setTrackMode(m: TrackingMode) {
        _trackMode.value = m
        if (m == TrackingMode.MANUAL) {
            pidPan.reset()
            pidTilt.reset()
        }
    }

    fun updateCameraFps(fps: Float) {
        _cameraFps.value = fps
    }

    fun getOdometry() = odometry

    fun resetOdometry() {
        odometry.reset()
        _pose.value = odometry.pose
    }

    // ══════════════════════════════════════════════════════════════════════
    // Калибровка (ИСПРАВЛЕНО: использует CalibrationResult)
    // ══════════════════════════════════════════════════════════════════════

    /** Применить результат калибровки лазера → обновить PID gains */
    fun applyCalibration(result: CalibrationResult) {
        _calibration.value = result
        if (result.isValid) {
            val kpPan = result.recommendedKp(CalibrationResult.Axis.PAN)
            val kpTilt = result.recommendedKp(CalibrationResult.Axis.TILT)
            pidPan.kp = kpPan
            pidTilt.kp = kpTilt
            Log.i(TAG, "Applied calibration: panKp=$kpPan, tiltKp=$kpTilt")
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    // Lifecycle
    // ══════════════════════════════════════════════════════════════════════

    override fun onCleared() {
        super.onCleared()
        disconnect()
        sender.close()
    }
}
