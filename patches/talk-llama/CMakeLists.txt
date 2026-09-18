# ATHENA patched CMakeLists.txt for whisper.cpp examples/talk-llama.
#
# Differences from upstream (both load-bearing — keep when syncing with upstream):
#   1. ONNX Runtime block: -DONNXRUNTIME_ROOT=<dir> enables the emotion2vec speech-
#      emotion tagger (defines ATHENA_EMOTION_ORT=1, links libonnxruntime, bakes the
#      lib dir into RPATH). Unset -> tagging compiles out, byte-identical baseline.
#   2. Threads::Threads is linked explicitly. The consciousness substrate
#      (athena_consciousness.h) runs a std::thread at 10 Hz, and on Linux a
#      std::thread with no pthread linkage throws system_error at construction
#      rather than failing at link time — a runtime failure on first launch is a
#      much worse way to find out. Upstream's ${CMAKE_THREAD_LIBS_INIT} is kept
#      alongside it; it is populated only if a parent scope already ran
#      find_package(Threads), which is not something to rely on here.
#   3. Conditional DeepSeek Sparse Attention source: current whisper.cpp syncs ship
#      llama-kv-cache-dsa.cpp/.h in this example; the ATHENA-pinned commit
#      (afa2ea544fb4) predates it. The EXISTS guard makes this one file build BOTH
#      trees — on a current pull the DSA translation unit is compiled (omitting it
#      breaks the link), on the pinned tree the guard no-ops.
#   4. Vision (r20): the mtmd/ subdirectory vendors libmtmd + clip from llama.cpp
#      commit 3e037f31 — the exact source of the talk-llama sync this tree carries
#      (whisper commit 46ca43d6, 2026-05-14), so the llama/ggml API delta is zero by
#      construction. Guarded by EXISTS: a tree without mtmd/ builds exactly as r19
#      (no vision symbols, ATHENA_VISION_MTMD undefined). mtmd/vendor/ carries the
#      stb_image + miniaudio single-header deps at the same commit.
#
# The ATHENA-custom headers (athena_memory.h, silero-endpointer.h,
# silero-turn-state.h, athena_consciousness.h, athena_report.h, athena_seam.h,
# athena_emotion.h, athena_calib.h) need no listing here: only .cpp files are
# compiled directly, and the headers are #included from the same directory.
#
#   4. r19.6: a SECOND target, athena-emotion-calibrate, from
#      athena-emotion-calibrate.cpp. Built only when ONNXRUNTIME_ROOT is set.


if (WHISPER_SDL2)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # Required by the consciousness substrate's tick thread (see header note 2).
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    file(GLOB SRC_MODELS models/*.cpp)

    # Newer llama syncs only (DeepSeek Sparse Attention KV cache) — see header note.
    set(SRC_DSA "")
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/llama-kv-cache-dsa.cpp)
        set(SRC_DSA llama-kv-cache-dsa.cpp)
    endif()

    # Vision: vendored libmtmd + clip (see header note 4). Optional by EXISTS guard.
    set(SRC_MTMD "")
    set(ATHENA_VISION_DEFS "")
    option(ATHENA_VISION_BUILD "Build the installed mtmd vision backend" ON)
    if(ATHENA_VISION_BUILD AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mtmd/mtmd.cpp)
        file(GLOB SRC_MTMD mtmd/*.cpp mtmd/models/*.cpp)
        # NO_AUDIO_FILES: whisper's libcommon already embeds miniaudio; a second
        # implementation in mtmd-helper collides at link (ma_atomic_global_lock).
        # Athena's audio path is whisper — mtmd carries images only.
        set(ATHENA_VISION_DEFS ATHENA_VISION_MTMD=1 ATHENA_MTMD_NO_AUDIO_FILES=1)
    endif()

    set(TARGET whisper-talk-llama)
    add_executable(${TARGET} talk-llama.cpp
        llama.cpp
        llama-adapter.cpp
        llama-arch.cpp
        llama-batch.cpp
        llama-chat.cpp
        llama-context.cpp
        llama-cparams.cpp
        llama-grammar.cpp
        llama-graph.cpp
        llama-hparams.cpp
        llama-impl.cpp
        llama-io.cpp
        llama-kv-cache.cpp
        llama-kv-cache-iswa.cpp
        llama-memory-recurrent.cpp
        llama-memory-hybrid.cpp
        llama-memory-hybrid-iswa.cpp
        llama-memory.cpp
        llama-mmap.cpp
        llama-model-loader.cpp
        llama-model-saver.cpp
        llama-model.cpp
        llama-quant.cpp
        llama-sampler.cpp
        llama-vocab.cpp
        unicode.cpp
        unicode-data.cpp
        ${SRC_DSA}
        ${SRC_MTMD}
        ${SRC_MODELS})
    target_include_directories(${TARGET} PRIVATE . ${SDL2_INCLUDE_DIRS})
    if(SRC_MTMD)
        target_include_directories(${TARGET} PRIVATE mtmd mtmd/vendor)
        target_compile_definitions(${TARGET} PRIVATE ${ATHENA_VISION_DEFS})
    endif()

    target_link_libraries(${TARGET} PRIVATE common common-sdl whisper ${SDL2_LIBRARIES}
        Threads::Threads ${CMAKE_THREAD_LIBS_INIT})

    # ────────────────────────────────────────────────────────────────────────
    # emotion2vec SER via ONNX Runtime (optional). Configure with:
    #   -DONNXRUNTIME_ROOT=<ATHENA>/onnxruntime-linux-x64-gpu_cuda12-1.27.0
    # If unset, whisper-talk-llama builds exactly as before (tagging compiled out).
    if(ONNXRUNTIME_ROOT)
        find_library(ONNXRUNTIME_LIB onnxruntime PATHS ${ONNXRUNTIME_ROOT}/lib REQUIRED)
        target_include_directories(${TARGET} PRIVATE ${ONNXRUNTIME_ROOT}/include)
        target_link_libraries(${TARGET} PRIVATE ${ONNXRUNTIME_LIB})
        target_compile_definitions(${TARGET} PRIVATE ATHENA_EMOTION_ORT=1)
        # bake the ORT lib dir into RPATH so the .so is found at runtime
        set_target_properties(${TARGET} PROPERTIES
            BUILD_RPATH   ${ONNXRUNTIME_ROOT}/lib
            INSTALL_RPATH ${ONNXRUNTIME_ROOT}/lib)
        message(STATUS "talk-llama: emotion2vec ONNX tagging ENABLED (${ONNXRUNTIME_LIB})")
    else()
        message(STATUS "talk-llama: ONNXRUNTIME_ROOT unset -> emotion tagging disabled")
    endif()

    install(TARGETS ${TARGET} RUNTIME)

    # ────────────────────────────────────────────────────────────────────────
    # r19.6: athena-emotion-calibrate — the interactive vocal-affect calibrator.
    #
    # Built ONLY when ONNXRUNTIME_ROOT is given, because without the emotion
    # model there is nothing to calibrate. It links whisper + common + common-sdl
    # + ONNX Runtime, exactly like whisper-talk-llama, and #includes the same
    # athena_emotion.h — so what it measures is the tagger she actually runs.
    #
    # This replaces patches/athena-emotion-calibrate.sh, which embedded ~450
    # lines of Python and violated the project's zero-Python-at-runtime rule.
    if(ONNXRUNTIME_ROOT)
        set(CALIB_TARGET athena-emotion-calibrate)
        add_executable(${CALIB_TARGET} athena-emotion-calibrate.cpp)
        target_include_directories(${CALIB_TARGET} PRIVATE . ${SDL2_INCLUDE_DIRS} ${ONNXRUNTIME_ROOT}/include)
        target_link_libraries(${CALIB_TARGET} PRIVATE common common-sdl whisper ${SDL2_LIBRARIES}
            Threads::Threads ${CMAKE_THREAD_LIBS_INIT} ${ONNXRUNTIME_LIB})
        target_compile_definitions(${CALIB_TARGET} PRIVATE ATHENA_EMOTION_ORT=1)
        set_target_properties(${CALIB_TARGET} PROPERTIES
            BUILD_RPATH   ${ONNXRUNTIME_ROOT}/lib
            INSTALL_RPATH ${ONNXRUNTIME_ROOT}/lib)
        install(TARGETS ${CALIB_TARGET} RUNTIME)
        message(STATUS "talk-llama: athena-emotion-calibrate ENABLED")
    endif()


    if(WIN32)
        # It requires Windows 8.1 or later for PrefetchVirtualMemory
        target_compile_definitions(${TARGET} PRIVATE -D_WIN32_WINNT=0x0602)
    endif()

    include(DefaultTargetOptions)
endif ()
