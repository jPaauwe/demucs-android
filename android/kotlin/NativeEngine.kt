package io.github.jpaauwe.demucsengine

/**
 * Demucs music source separation on the phone (htdemucs_6s: drums, bass,
 * other, vocals, guitar, piano), on every CPU core and the GPU.
 *
 * Copy this file into your app and build the engine with SDENGINE_JNI=ON.
 * If you change the package or class name, set SDENGINE_JNI_CLASS to match
 * (e.g. "com/example/audio/NativeEngine").
 *
 * Typical use, on a background thread:
 *   NativeEngine.loadModel(weights)              // 5c90dfd2.safetensors
 *   NativeEngine.prepareInput(rawF32, 48000, 2, prepared) { f, _ -> true }
 *   val rms = NativeEngine.separate(prepared, outDir, threads = 2) { f, stage -> !cancelled }
 *   // outDir/drums.wav, bass.wav, other.wav, vocals.wav, guitar.wav, piano.wav
 */
object NativeEngine {
    /** Progress from the engine. Return false to ask it to stop. */
    fun interface ProgressListener {
        fun onProgress(fraction: Float, stage: String): Boolean
    }

    init {
        System.loadLibrary("sdengine_jni")
    }

    /** The six sources, in the order the model produces them. */
    val SOURCES: List<String> = listOf("drums", "bass", "other", "vocals", "guitar", "piano")

    @JvmStatic private external fun nativeLoadModel(path: String): String
    @JvmStatic private external fun nativePrepareInput(
        raw: String, sampleRate: Int, channels: Int, out: String, listener: ProgressListener?,
    ): String
    @JvmStatic private external fun nativeAnalyze(path: String): DoubleArray
    @JvmStatic private external fun nativeSeparate(
        prepared: String, outDir: String, write: BooleanArray, threads: Int, computeThreads: Int,
        gpu: Boolean, listener: ProgressListener?,
    ): DoubleArray
    @JvmStatic private external fun nativeComputeInfo(computeThreads: Int, gpu: Boolean): String
    @JvmStatic private external fun nativeCancel()

    /**
     * Loads the weights once (no-op when the same file is already loaded):
     * the .safetensors file Demucs publishes, or a demucs.cpp ggml file.
     * Throws IllegalStateException with a readable message on failure.
     */
    @Synchronized
    fun loadModel(path: String) {
        val err = nativeLoadModel(path)
        if (err.isNotEmpty()) throw IllegalStateException(err)
    }

    /**
     * Raw interleaved float32 PCM at any rate and channel count -> the
     * engine's input: interleaved stereo float32 at 44.1 kHz.
     * Throws CancellationException when cancelled.
     */
    fun prepareInput(raw: String, sampleRate: Int, channels: Int, out: String, listener: ProgressListener? = null) {
        val err = nativePrepareInput(raw, sampleRate, channels, out, listener)
        if (err.isNotEmpty()) throw IllegalStateException(err)
    }

    data class InputStats(val durationSec: Double, val peakDb: Double?, val lufs: Double?)

    /** Length, sample peak (dBFS) and BS.1770 loudness (LUFS) of a prepared input. */
    fun analyze(prepared: String): InputStats {
        val v = nativeAnalyze(prepared)
        return InputStats(v[0], v[1].takeUnless { it.isNaN() }, v[2].takeUnless { it.isNaN() })
    }

    /**
     * Writes <outDir>/<source>.wav (16-bit stereo, 44.1 kHz) for each
     * selected source and returns each one's RMS, in [SOURCES] order.
     * [threads] segments run at once (each needs about 0.6 GB); the maths
     * inside them uses [computeThreads] cores (0 = all) and the GPU when
     * [gpu] is on. Throws CancellationException when cancelled,
     * RuntimeException on failure.
     */
    fun separate(
        prepared: String, outDir: String, selected: Set<String> = SOURCES.toSet(), threads: Int = 2,
        computeThreads: Int = 0, gpu: Boolean = true, listener: ProgressListener? = null,
    ): DoubleArray {
        val write = BooleanArray(SOURCES.size) { SOURCES[it] in selected }
        return nativeSeparate(prepared, outDir, write, threads, computeThreads, gpu, listener)
    }

    /** What separation runs on, e.g. "8 cores, GPU: Adreno (TM) 750 (Vulkan 1.3)", or why the GPU is not used. */
    fun computeInfo(gpu: Boolean = true, computeThreads: Int = 0): String = nativeComputeInfo(computeThreads, gpu)

    /** Asks a running prepareInput() or separate() to stop. */
    fun cancel() = nativeCancel()
}
