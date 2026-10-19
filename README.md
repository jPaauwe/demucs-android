# Demucs Android engine

Music source separation on Android phones: **Demucs v4** (`htdemucs_6s`, by
Meta) in C++, split into **drums, bass, other, vocals, guitar and piano**,
running entirely on the device on **every CPU core and the GPU**.

It is the separation engine of
[StemDeck for Android](https://github.com/jPaauwe/stemdeck-android), as a
library you can build into your own app. It started from
[demucs.cpp](https://github.com/sevagh/demucs.cpp) (a C++ implementation of
Demucs), reworked for phones:

- **Memory:** the transposed convolutions run without the im2col matrix
  upstream builds, which cut one inference thread from about 1.7 GB to about
  0.6 GB.
- **Speed:** faster group/layer norms, GLU, GELU, layer scale, attention
  (block-wise, no full score matrix) and convolutions (one product per kernel
  tap), and blocked layout transposes.
- **Demucs' own way of applying the model:** the whole song is normalized
  with its own statistics, cut into the model's 7.8 s segments with 25%
  overlap and a time shift, and blended back with triangular weights, as
  PyTorch Demucs does. Memory stays flat however long the song is.
- **Every core:** a shared thread pool. The segments that fit in memory run
  side by side, and the heavy maths inside each one is split into small
  chunks that any idle core takes, so the fast and slow cores of a phone chip
  each do what they can.
- **GPU:** Vulkan compute shaders (`src/compute/shaders`) run the
  transformer (projections, attention, feed-forward) and the large
  convolution products. Vulkan is loaded at run time; the GPU checks itself
  against the CPU on first use, and whenever it is missing, fails, or is busy
  with another segment, that work runs on the CPU.
- **Demucs v4.1.0 weights, as published:** it reads the `.safetensors` file
  the Demucs project publishes on the HuggingFace hub directly. It also reads
  demucs.cpp's ggml files.

The results are the same as upstream demucs.cpp's, to float rounding, given
the same time shift. CI checks this, and checks the GPU against the CPU.

## Weights

Download Demucs v4.1.0's `htdemucs_6s` weights (55 MB, MIT):

```
https://huggingface.co/adefossez/HTDemucs-6s/resolve/3c5ee475be622df764938de97e4281a7b07ffa58/5c90dfd2.safetensors
SHA-256 d2a1745f0744721f6b8ca5bf469b67c651ea5ed1b52998cab033b2158609d411
```

and pass the file to `load_model` as it is.

## Using it in an Android app

```sh
git submodule add https://github.com/jPaauwe/demucs-android third_party/demucs-android
git submodule update --init --recursive
```

In your app's `CMakeLists.txt` (NDK r27, `minSdk` 29 or higher, arm64-v8a):

```cmake
set(SDENGINE_JNI ON)                                       # libsdengine_jni.so
set(SDENGINE_JNI_CLASS "com/example/audio/NativeEngine")   # your class
add_subdirectory(third_party/demucs-android)
```

Then copy `android/kotlin/NativeEngine.kt` into your app, in the package you
named above. From a background thread:

```kotlin
NativeEngine.loadModel(weightsFile.path)                     // the .safetensors file
NativeEngine.prepareInput(rawF32.path, 48000, 2, prepared.path)   // decoded PCM -> 44.1 kHz stereo
println(NativeEngine.computeInfo())                          // "8 cores, GPU: Adreno (TM) 750 (Vulkan 1.3)"
val rms = NativeEngine.separate(prepared.path, outDir.path, threads = 2) { fraction, stage ->
    !cancelled                                               // return false to stop
}
// outDir/drums.wav, bass.wav, other.wav, vocals.wav, guitar.wav, piano.wav
```

Decode the audio with Android's `MediaCodec` into raw interleaved float32
first. `prepareInput` resamples it to 44.1 kHz stereo.

`threads` is how many segments run at once. Each needs about 0.6 GB, so 1
on 4 GB phones, 2 on 8 GB and 3 on 12 GB is a good start. The maths inside
them uses every core regardless. Pass `gpu = false` for CPU only.

Or use the C++ API directly (`src/engine.hpp`): link
`demucs_android::engine` and call `sdengine::load_model`,
`prepare_input`, `separate` and `request_cancel`.

## Building on a computer

```sh
git clone --recursive https://github.com/jPaauwe/demucs-android
cmake -S demucs-android -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/sdengine_cli separate 5c90dfd2.safetensors prepared.f32 out/
```

This needs the Vulkan headers (`libvulkan-dev`, or `-DVULKAN_HEADERS=<dir>`).
The tools are:

- `sdengine_cli` (prepare, analyze, separate),
- `sdbench_cli` (time one segment per operation, make a test song, compare
  outputs),
- `sdgpu_test` (GPU against CPU; `SD_GPU_ALLOW_CPU=1` allows a software GPU
  such as llvmpipe or SwiftShader).

`SD_NO_GPU=1` and `SD_THREADS=n` control the CLI.

## Measurements

On a **Samsung Galaxy S24** (SM-S921B: Exynos 2400, 10 cores, Xclipse 940
GPU, 8 GB, Android 16), a 1:26 song, all six stems, 6 segments at once,
measured with StemDeck Compare (build 25), run one after the other:

| Engine | Time | Song length | Speed-up | Peak memory | Thermal status |
|---|---|---|---|---|---|
| StemDeck builds 4–10: 24 s chunks, one core per chunk | 150.1 s | 1.73x | 1.00x | 2.58 GB | none → light |
| StemDeck build 13: Demucs' 7.8 s segments, one core per segment | 66.8 s | 0.77x | 2.25x | 3.54 GB | light → moderate |
| **this engine**: v4.1.0 weights, every core, GPU on | **45.2 s** | **0.52x** | **3.32x** | 3.34 GB | moderate |

This engine separates the song in about half its length, 1.5x faster than
build 13 even though the phone was already warm when it ran. Its stems are
identical to build 13's (101 dB SNR over all stems). Builds 4–10 differ from
both by about 18 dB, because they cut the song into 24 s chunks with a time
shift each; that is the same kind of difference two runs of PyTorch Demucs
show.

On a 2-core x86 computer, 20-second song, all six stems:

| Engine | Time | Peak memory |
|---|---|---|
| upstream demucs.cpp (1 thread) | 267 s | 1.9 GB |
| this engine before the thread pool and GPU, 2 segments at once | 63 s | 1.5 GB |
| this engine, 1 segment at once using both cores | 70 s (vs 103 s without the pool) | 0.8 GB |

The GPU shaders are portable but not yet tuned for any particular phone GPU
(Adreno, Mali, Xclipse), so compare `gpu = false` against the default on
yours.

## Layout

- `src/engine.*`: the API (decoding is the app's job; resampling, loudness,
  separation and WAV writing are here).
- `src/demucs/`: the model, from demucs.cpp, with each change marked
  "StemDeck Android".
- `src/compute/`: thread pool, matrix-product dispatcher, Vulkan backend and
  shaders. `shaders/embed.py` regenerates `spirv.h` after changing a shader.
- `src/safetensors.*`: the weights reader.
- `android/`: the JNI binding and its Kotlin class.
- `tools/`, `bench/`, `scripts/`: desktop tools and tests.

## Licences

- This repository: MIT (`LICENSE`).
- demucs.cpp: MIT, © Sevag H (`src/demucs/LICENSE`).
- Demucs and the `htdemucs_6s` weights: MIT, © Meta Platforms.
- Eigen (`third_party/eigen`, a submodule): MPL 2.0.
