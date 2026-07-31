package com.example.amplitude

import android.content.Context
import android.net.Uri
import java.io.File

/**
 * Обёртка над libamplitude.so: аудиофайл -> значения амплитуды (Int, PCM16: -32768..32767).
 *
 * Форматы: MP3 — встроенным minimp3; M4A/AAC (в т.ч. HE-AAC), FLAC, OGG, WAV —
 * системным декодером Android (MediaCodec).
 *
 * Все вызовы блокирующие — выполнять на фоновом диспетчере (Dispatchers.IO).
 */
object Amplitude {

    init { System.loadLibrary("amplitude") }

    // режим канала
    const val CH_MIX = 0      // (L+R)/2
    const val CH_LEFT = 1
    const val CH_RIGHT = 2
    const val CH_BOTH = 3     // два значения на точку: L, R (интерливед)

    // свёртка окна (действует, когда окно больше одного отсчёта)
    const val REDUCE_PEAK = 0
    const val REDUCE_RMS = 1
    const val REDUCE_AVG = 2

    // выбор декодера
    const val BACKEND_AUTO = 0
    const val BACKEND_MINIMP3 = 1
    const val BACKEND_MEDIA = 2

    /**
     * @param sampleRate     частота дискретизации, Гц
     * @param channels       каналов в исходном файле (1 или 2)
     * @param valuesPerPoint 1, либо 2 для CH_BOTH со стереофайлом
     * @param totalFrames    всего отсчётов в источнике (до свёртки), 0 если неизвестно
     * @param blockSamples   фактический размер окна в отсчётах
     * @param values         значения; точек = values.size / valuesPerPoint
     */
    class Amplitudes(
        val sampleRate: Int,
        val channels: Int,
        val valuesPerPoint: Int,
        val totalFrames: Int,
        val blockSamples: Int,
        val values: IntArray,
    ) {
        val points: Int get() = if (valuesPerPoint > 0) values.size / valuesPerPoint else 0

        /** Длительность одной точки в миллисекундах. */
        val pointDurationMs: Double
            get() = if (sampleRate > 0) 1000.0 * blockSamples / sampleRate else 0.0

        /** Значение точки в dBFS (-inf для нуля). */
        fun dbfs(index: Int, channel: Int = 0): Double {
            val v = kotlin.math.abs(values[index * valuesPerPoint + channel]).toDouble()
            return if (v <= 0.0) Double.NEGATIVE_INFINITY
            else 20.0 * kotlin.math.log10(v / 32768.0)
        }
    }

    class DecodeException(message: String) : RuntimeException(message)

    /**
     * @param intervalMs  окно в миллисекундах (0 = не использовать)
     * @param block       окно в отсчётах; игнорируется, если задан intervalMs
     * @param limitPoints не больше стольких точек (0 = без ограничения)
     */
    @JvmOverloads
    fun fromFile(
        file: File,
        intervalMs: Int = 0,
        block: Int = 1,
        channel: Int = CH_MIX,
        reduce: Int = REDUCE_PEAK,
        absolute: Boolean = false,
        limitPoints: Int = 0,
        backend: Int = BACKEND_AUTO,
    ): Amplitudes {
        val info = IntArray(5)
        val values = nativeDecodeFile(
            file.absolutePath, channel, reduce, block, intervalMs,
            absolute, limitPoints, backend, info
        ) ?: fail(file.name)
        return build(info, values)
    }

    /** Декодирование из content:// (SAF, MediaStore) — без прав на storage. */
    @JvmOverloads
    fun fromUri(
        context: Context,
        uri: Uri,
        intervalMs: Int = 0,
        block: Int = 1,
        channel: Int = CH_MIX,
        reduce: Int = REDUCE_PEAK,
        absolute: Boolean = false,
        limitPoints: Int = 0,
        backend: Int = BACKEND_AUTO,
    ): Amplitudes {
        val info = IntArray(5)
        val values = context.contentResolver.openFileDescriptor(uri, "r").use { pfd ->
            pfd ?: throw DecodeException("не удалось открыть $uri")
            nativeDecodeFd(
                pfd.fd, 0L, pfd.statSize, channel, reduce, block, intervalMs,
                absolute, limitPoints, backend, info
            )
        } ?: fail(uri.toString())
        return build(info, values)
    }

    /** Декодирование из массива в памяти (сеть, assets, кэш). */
    @JvmOverloads
    fun fromBytes(
        data: ByteArray,
        offset: Int = 0,
        length: Int = data.size - offset,
        intervalMs: Int = 0,
        block: Int = 1,
        channel: Int = CH_MIX,
        reduce: Int = REDUCE_PEAK,
        absolute: Boolean = false,
        limitPoints: Int = 0,
        backend: Int = BACKEND_AUTO,
    ): Amplitudes {
        val info = IntArray(5)
        val values = nativeDecodeBytes(
            data, offset, length, channel, reduce, block, intervalMs,
            absolute, limitPoints, backend, info
        ) ?: fail("буфер ($length байт)")
        return build(info, values)
    }

    private fun fail(what: String): Nothing {
        val err = nativeLastError().ifBlank { "неизвестная ошибка" }
        throw DecodeException("$what: $err")
    }

    private fun build(info: IntArray, values: IntArray) =
        Amplitudes(info[0], info[1], info[2], info[3], info[4], values)

    // --- native ---------------------------------------------------------

    @JvmStatic
    private external fun nativeDecodeFile(
        path: String, channel: Int, reduce: Int, block: Int, intervalMs: Int,
        absolute: Boolean, limitPoints: Int, backend: Int, info: IntArray,
    ): IntArray?

    @JvmStatic
    private external fun nativeDecodeFd(
        fd: Int, offset: Long, size: Long, channel: Int, reduce: Int, block: Int,
        intervalMs: Int, absolute: Boolean, limitPoints: Int, backend: Int, info: IntArray,
    ): IntArray?

    @JvmStatic
    private external fun nativeDecodeBytes(
        data: ByteArray, offset: Int, length: Int, channel: Int, reduce: Int, block: Int,
        intervalMs: Int, absolute: Boolean, limitPoints: Int, backend: Int, info: IntArray,
    ): IntArray?

    @JvmStatic
    private external fun nativeLastError(): String
}
