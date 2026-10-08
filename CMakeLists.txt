cmake_minimum_required(VERSION 3.16)
project(hevc-decode VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_WASM "Build WebAssembly target" OFF)
option(HEVC_DEBUG "Enable debug logging" OFF)

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Debug : AddressSanitizer
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT BUILD_WASM)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Debug logging
if(HEVC_DEBUG OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(HEVC_DEBUG=1)
endif()

# Library
add_library(hevc-decode-lib
    src/bitstream/bitstream_reader.cpp
    src/bitstream/nal_parser.cpp
    src/common/picture.cpp
    src/common/thread_pool.cpp
    src/syntax/profile_tier_level.cpp
    src/syntax/vps.cpp
    src/syntax/sps.cpp
    src/syntax/pps.cpp
    src/syntax/slice_header.cpp
    src/syntax/parameter_sets.cpp
    src/syntax/sei.cpp
    src/syntax/vui.cpp
    src/decoding/cabac.cpp
    src/decoding/syntax_elements.cpp
    src/decoding/coding_tree.cpp
    src/decoding/residual_coding.cpp
    src/decoding/transform.cpp
    src/decoding/intra_prediction.cpp
    src/decoding/inter_prediction.cpp
    src/decoding/interpolation.cpp
    src/decoding/dpb.cpp
    src/decoding/decoder.cpp
    src/filters/deblocking.cpp
    src/filters/sao.cpp
    src/wasm/hevc_api.cpp
)

target_include_directories(hevc-decode-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# CLI executable
add_executable(hevc-decode
    src/main.cpp
)

target_link_libraries(hevc-decode PRIVATE hevc-decode-lib)

# WASM-specific settings
if(BUILD_WASM)
    # WASM target: build the C API as a JS+WASM module (not the CLI)
    add_executable(hevc-wasm src/wasm/hevc_api.cpp)
    target_link_libraries(hevc-wasm PRIVATE hevc-decode-lib)
    set_target_properties(hevc-wasm PROPERTIES
        OUTPUT_NAME "hevc-decode"
        SUFFIX ".js"
    )
    # Exception handling. The decoder contains bitstream errors with C++
    # exceptions (over-read, Exp-Golomb overflow, ...). Native WebAssembly
    # exception handling (legacy proposal: Chrome 95+, Firefox 100+,
    # Safari 15.2+ — below the SIMD floor this build already requires) makes a
    # try block free until something throws; the JS-based emulation routes
    # every call that may throw through an invoke_* trampoline into JS.
    option(HEVC_WASM_NATIVE_EH "Use native WebAssembly exception handling" ON)
    if(HEVC_WASM_NATIVE_EH)
        set(HEVC_WASM_EH_COMPILE -fwasm-exceptions "SHELL:-s WASM_LEGACY_EXCEPTIONS=1")
        set(HEVC_WASM_EH_LINK -fwasm-exceptions "SHELL:-s WASM_LEGACY_EXCEPTIONS=1")
    else()
        set(HEVC_WASM_EH_COMPILE -fexceptions)
        set(HEVC_WASM_EH_LINK "SHELL:-s NO_DISABLE_EXCEPTION_CATCHING=1")
    endif()
    # SIMD 128-bit for the whole project in WASM mode.
    # -msse2 lets emscripten lower our portable SSE2 kernels (<emmintrin.h>) to
    # wasm128 — the same intrinsics compile natively on x86-64, so the oracle
    # tests verify the exact code that ships to WASM.
    target_compile_options(hevc-decode-lib PRIVATE ${HEVC_WASM_EH_COMPILE} -msimd128 -msse2)
    target_link_options(hevc-wasm PRIVATE ${HEVC_WASM_EH_LINK})
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(hevc-wasm PRIVATE -O0 -g ${HEVC_WASM_EH_COMPILE} -msimd128)
        # Debug: keep emscripten runtime sanity checks (stack cookies, bounds).
        target_link_options(hevc-wasm PRIVATE "SHELL:-s ASSERTIONS=1")
    else()
        # Release: drop emscripten runtime sanity checks (ASSERTIONS adds stack
        # cookie / bounds checks that cost real cycles in the hot decode loop).
        # Bit-exact (verified vs oracle); ~3% faster decode at 1080p and 4K.
        # NB: -flto was measured here and REGRESSED decode ~3% (emcc's cross-TU
        # inlining bloats the hot path / hurts I-cache), so it is intentionally off.
        target_compile_options(hevc-wasm PRIVATE -O3 ${HEVC_WASM_EH_COMPILE} -msimd128)
        target_link_options(hevc-wasm PRIVATE "SHELL:-s ASSERTIONS=0")
    endif()
    target_link_options(hevc-wasm PRIVATE
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=HEVCDecoderModule"
        "SHELL:-s STACK_SIZE=4194304"
        "SHELL:-s EXPORTED_FUNCTIONS=[_hevc_decoder_create,_hevc_decoder_destroy,_hevc_decoder_decode,_hevc_decoder_get_frame_count,_hevc_decoder_get_frame,_hevc_decoder_get_info,_hevc_decoder_get_recovery_point,_hevc_decoder_feed,_hevc_decoder_feed_tagged,_hevc_decoder_get_stats,_hevc_decoder_set_error_policy,_hevc_decoder_drain,_hevc_decoder_get_drained_frame,_hevc_decoder_flush,_hevc_decoder_reset,_hevc_decoder_discontinuity,_malloc,_free]"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[ccall,cwrap,getValue,HEAPU8,HEAPU16]"
    )
    # Don't build the native CLI in WASM mode
    set_target_properties(hevc-decode PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# Tests
if(BUILD_TESTS AND NOT BUILD_WASM)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(hevc_tests
        tests/unit/test_bitstream_reader.cpp
        tests/unit/test_nal_parser.cpp
        tests/unit/test_picture.cpp
        tests/unit/test_parameter_sets.cpp
        tests/unit/test_cabac.cpp
        tests/unit/test_incremental.cpp
        tests/unit/test_reset.cpp
        tests/unit/test_small_transforms.cpp
        tests/unit/test_ref_lists.cpp
        tests/unit/test_interp_conceal.cpp
        tests/unit/test_slice_ltrp_sps.cpp
        tests/unit/test_sps_validate_dims.cpp
        tests/unit/test_scaling_list_scan.cpp
        tests/unit/test_dequant_scaling_444.cpp
        tests/unit/test_random_access.cpp
        tests/unit/test_decoder_resync.cpp
        tests/unit/test_sei.cpp
        tests/unit/test_monochrome_output.cpp
        tests/unit/test_output_process.cpp
        tests/unit/test_validation.cpp
        tests/unit/test_nonvcl_tolerance.cpp
        tests/unit/test_picture_assembly.cpp
        tests/unit/test_tagged_output.cpp
        tests/unit/test_discontinuity.cpp
        tests/unit/test_error_policy.cpp
        tests/unit/test_corruption_corpus.cpp
        tests/unit/test_poc.cpp
    )

    target_link_libraries(hevc_tests PRIVATE
        hevc-decode-lib
        GTest::gtest_main
    )

    target_compile_definitions(hevc_tests PRIVATE
        FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/conformance/fixtures"
    )

    # Fuzz smoke: thousands of mutated fixtures under every policy/feeding
    # mode, in one ASan process (tests/fuzz/fuzz_smoke.cpp).
    add_executable(hevc_fuzz_smoke tests/fuzz/fuzz_smoke.cpp)
    target_link_libraries(hevc_fuzz_smoke PRIVATE hevc-decode-lib)
    target_compile_definitions(hevc_fuzz_smoke PRIVATE
        FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/conformance/fixtures")
    add_test(NAME fuzz_smoke COMMAND hevc_fuzz_smoke --iterations 300 --quiet)
    set_tests_properties(fuzz_smoke PROPERTIES LABELS "fuzz" TIMEOUT 900
        ENVIRONMENT "HEVC_DEBUG_FILTER=none")

    include(GoogleTest)
    # A non-terminating decode loop must fail the suite, not stall it.
    # HEVC_DEBUG_FILTER=none: Debug builds log every category by default, which
    # dominates the suite's run time and output.
    gtest_discover_tests(hevc_tests PROPERTIES TIMEOUT 300 ENVIRONMENT "HEVC_DEBUG_FILTER=none")

    # Oracle tests — compare decoded YUV MD5 against reference
    # These tests require the decoder to produce YUV output (-o flag)
    # They are labeled "oracle" so they can be run separately: ctest -L oracle
    set(FIXTURES_DIR "${CMAKE_SOURCE_DIR}/tests/conformance/fixtures")
    set(ORACLE_SCRIPT "${CMAKE_SOURCE_DIR}/tools/oracle_test.sh")

    # All oracle tests skip gracefully when decoder can't produce output yet
    # Exit code 2 = SKIP (decoder not implemented for this feature)

    # Toy bitstreams — ultra-simple for step-by-step debugging (64x64, 1 CTU)
    add_test(NAME oracle_toy_qp30
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/toy_qp30.265 64 64
            9f59878470d904dc2e162d3d191611af $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_toy_qp30 PROPERTIES LABELS "oracle;phase4;toy"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_toy_qp45
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/toy_qp45.265 64 64
            52ca5108bd4354383a9d29d96cf64b47 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_toy_qp45 PROPERTIES LABELS "oracle;phase4;toy"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_toy_qp10
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/toy_qp10.265 64 64
            ff469ce7e872152490b9b39b0d05dd7b $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_toy_qp10 PROPERTIES LABELS "oracle;phase4;toy"
        SKIP_RETURN_CODE 2)

    # Phase 4 — I-only, no filters
    add_test(NAME oracle_i_64x64_qp22
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_qp22.265 64 64
            48c64cd2de381113913149e92065d66c $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_i_64x64_qp22 PROPERTIES LABELS "oracle;phase4"
        SKIP_RETURN_CODE 2)

    # Open-GOP CRA + RASL leading pictures (random access, feature F1).
    # Decoded from the start: the mid-stream CRA has NoRaslOutputFlag == 0, so its
    # RASL leading pictures ARE decoded and output. Must stay byte-identical to
    # the trusted ffmpeg reference — this guards the decoder-level NoRaslOutputFlag
    # refactor against regressing the normal full-GOP path.
    add_test(NAME oracle_opengop_cra_rasl
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/opengop_cra_rasl.265 176 144
            9fba759b58ff8cd4a9a7ac5473cc1b1b $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_opengop_cra_rasl PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Upstream open-GOP fixtures (§8.1 RASL PicOutputFlag override). The CRA of
    # opengop_qcif_12f is mid-stream (NoRaslOutputFlag = 0, RASL output); the cut
    # stream opens on that CRA, whose three RASL pictures must not be output.
    add_test(NAME oracle_opengop_qcif_12f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/opengop_qcif_12f.265 176 144
            06e53846ff51cf35cadc97a080147e82 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_opengop_qcif_12f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_opengop_qcif_cra_first
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/opengop_qcif_cra_first.265 176 144
            9c249fb00174f2f6e6fbb3457c10679b $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_opengop_qcif_cra_first PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Multi-slice pictures (x265 --slices 3, 32x32 CTBs; and 16x16 CTBs with
    # WPP, 3 slices of 3 CTB rows each with 2 entry points). Guards picture
    # assembly across slices and per-slice reference lists.
    add_test(NAME oracle_multislice_qcif_8f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/multislice_qcif_8f.265 176 144
            a3248d9042a15320f483931c567ec06c $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_multislice_qcif_8f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_multislice_wpp_qcif_8f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/multislice_wpp_qcif_8f.265 176 144
            ef35793ec7e91f163df00ca6c5cbe820 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_multislice_wpp_qcif_8f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Camera-like content (static textured background, one moving object;
    # x265 P-only, 3 references). Used by the concealment tests, where
    # substituting the nearest picture must beat mid-grey prediction.
    add_test(NAME oracle_camera_qcif_12f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/camera_qcif_12f.265 176 144
            bdd9970bde02ad1ae82bede0a56a3de8 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_camera_qcif_12f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # JCT-VC conformance streams (ITU-T H.265.1) that guard specific fixes:
    # PCM alignment / QP / loop-filter skipping (ipcm_C, ipcm_D), parallel
    # merge level (PMERGE_B), constrained intra prediction (CIP_A), prevTid0Pic
    # (POC_A), and merge pruning after the 8x4 bi-prediction restriction (the
    # first two pictures of MAXBINS_B).
    add_test(NAME oracle_jctvc_ipcm_C_NEC_3
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_ipcm_C_NEC_3.265 416 240
            c3e74c399b73a5ab2dbd20523f583464 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_ipcm_C_NEC_3 PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_ipcm_D_NEC_3
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_ipcm_D_NEC_3.265 416 240
            aa64a16240064bc2a90fadf979a62a7b $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_ipcm_D_NEC_3 PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_PMERGE_B_TI_3
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_PMERGE_B_TI_3.265 416 240
            7d0e57ebdbbc6633c972083a17d97550 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_PMERGE_B_TI_3 PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_CIP_A_Panasonic_3
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_CIP_A_Panasonic_3.265 416 240
            4cbf601ba98d63f642defab5eaa12c8d $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_CIP_A_Panasonic_3 PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_POC_A_Bossen_3
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_POC_A_Bossen_3.265 416 240
            faf33bec64d3e58c426ed2772903a0bf $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_POC_A_Bossen_3 PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_MAXBINS_B_TI_5_2f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_MAXBINS_B_TI_5_2f.265 416 240
            7522b8154651a696e053057f440cd11f $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_MAXBINS_B_TI_5_2f PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)
    add_test(NAME oracle_jctvc_DSLICE_A_HHI_5_1f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/jctvc_DSLICE_A_HHI_5_1f.265 1920 1080
            7a7bedeed541f7f5185d33139b378fb1 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_jctvc_DSLICE_A_HHI_5_1f PROPERTIES LABELS "oracle;conformance"
        SKIP_RETURN_CODE 2)

    # Recovery-point SEI (gradual decoding refresh, feature F2). An intra-refresh
    # clip with x265 idr-recovery-sei: the IDR carries a recovery_point PREFIX_SEI.
    # SEI is now parsed but pixel output is unchanged — this guards that the SEI
    # subsystem does not perturb decoded samples (must stay byte-identical to the
    # trusted ffmpeg reference).
    add_test(NAME oracle_sei_recovery_point
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/sei_recovery_point.265 176 144
            87897c661d1b170cf2c8d879777c9f0d $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_sei_recovery_point PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Explicit (non-default) scaling list — exercises the up-right diagonal
    # scan -> raster mapping in ScalingListData::parse (feature C5).
    add_test(NAME oracle_i_64x64_scaling_list
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_scaling_list.265 64 64
            7b14e2d4f20b2525ee9f038d4ded62fb $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_i_64x64_scaling_list PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Monochrome 4:0:0 (feature F3 — IR/thermal cameras). i400 luma-only clip with
    # adaptive QP. Guards two monochrome bugs: the chroma-CBF root seed must not leak
    # into the cu_qp_delta condition (CABAC desync), and the output path must report
    # 0-width chroma / null chroma pointers (no read into the empty chroma buffers).
    add_test(NAME oracle_mono_qcif_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/mono_qcif_4f.265 176 144
            3c4e47dcac037796dfa665ab9e51e822 $<TARGET_FILE:hevc-decode> gray)
    set_tests_properties(oracle_mono_qcif_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:2:2 chroma (feature M1 — Main 4:2:2 / Rext). QCIF clip with I + P frames,
    # deblocking + SAO. Guards the 4:2:2 transform path: two stacked chroma TBs per
    # luma TB (deferred-CBF inheritance, per-block residual/dequant/transform), the
    # luma-space intra reference-availability (chroma CTB height is full in 4:2:2),
    # and the doubled vertical chroma motion-vector for inter prediction.
    add_test(NAME oracle_i_p_qcif_422_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_422_4f.265 176 144
            ca06427d6ae6af68127f3812b75aa9b5 $<TARGET_FILE:hevc-decode> yuv422p)
    set_tests_properties(oracle_i_p_qcif_422_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:2:2 10-bit (Main 4:2:2 10). Same clip as above at yuv422p10le — guards the
    # 4:2:2 path on the uint16 sample storage / 10-bit output writer.
    add_test(NAME oracle_i_p_qcif_422_10b_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_422_10b_4f.265 176 144
            7df5ccbfcca98ff8adce6b7321420f41 $<TARGET_FILE:hevc-decode> yuv422p10le)
    set_tests_properties(oracle_i_p_qcif_422_10b_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:2:2 high-QP (qp=42). Guards spec 8.6.1 chroma-QP derivation: for
    # ChromaArrayType != 1 (4:2:2/4:4:4), qPc = Min(qPi, 51) — NOT the 4:2:0
    # mapping table. The table is identity for qPi <= 29, so only a high-QP
    # 4:2:2 stream (qPi > 30) exercises the divergent clamp path.
    add_test(NAME oracle_i_p_qcif_422_hq_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_422_hq_4f.265 176 144
            52162b65507ff80be5b3aa6819071cf6 $<TARGET_FILE:hevc-decode> yuv422p)
    set_tests_properties(oracle_i_p_qcif_422_hq_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:4:4 chroma (feature M1 — Main 4:4:4 / Rext). QCIF clip with I + P frames,
    # SAO on. Guards the 4:4:4 transform path: chroma co-sited at full luma
    # resolution, so (a) intra_chroma_pred_mode is signalled per luma PU — four
    # modes for an intra-NxN CU, not one — and (b) every transform leaf carries its
    # own chroma TB (chroma is NOT deferred to blkIdx==3 as in 4:2:0/4:2:2). The
    # clip is NxN-heavy (240 intra-NxN CUs) to exercise both paths; before the fix
    # the per-leaf chroma residual bins went unread and desynced CABAC.
    add_test(NAME oracle_i_p_qcif_444_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_444_4f.265 176 144
            8a3d86c4a2452f4aa7f3668f6f710679 $<TARGET_FILE:hevc-decode> yuv444p)
    set_tests_properties(oracle_i_p_qcif_444_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:4:4 10-bit (Main 4:4:4 10). Same clip at yuv444p10le — guards the 4:4:4
    # path on uint16 sample storage / 10-bit output.
    add_test(NAME oracle_i_p_qcif_444_10b_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_444_10b_4f.265 176 144
            5192c41e701ab377e96e9de2ee3133f4 $<TARGET_FILE:hevc-decode> yuv444p10le)
    set_tests_properties(oracle_i_p_qcif_444_10b_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # 4:4:4 high-quality (qp=6) — maximal intra-NxN and dense per-leaf chroma
    # residual, the worst case for the per-leaf chroma desync.
    add_test(NAME oracle_i_p_qcif_444_hq_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_444_hq_4f.265 176 144
            fe6acf6ed144c98e643d7485debe9c62 $<TARGET_FILE:hevc-decode> yuv444p)
    set_tests_properties(oracle_i_p_qcif_444_hq_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # transform_skip (§8.6.4.2). I+P clips with transform_skip_enabled_flag and
    # transform-skipped 4x4 blocks. Guards the skip residual shift:
    # 15 - BitDepth - Log2(nTbS) (the previous code used 15 - BitDepth, omitting
    # the -Log2(nTbS) term and scaling every skip block down by 4x). Both decode
    # wrong before the fix. 4:2:0 (common) + 4:4:4 (per-leaf chroma + skip).
    add_test(NAME oracle_i_p_qcif_tskip_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_tskip_4f.265 176 144
            e6f8200e4a8d09692e5fbfd51f271be4 $<TARGET_FILE:hevc-decode> yuv420p)
    set_tests_properties(oracle_i_p_qcif_tskip_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_i_p_qcif_444_tskip_4f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_p_qcif_444_tskip_4f.265 176 144
            25e77cb30a2dddf7c8e47570c8b83a61 $<TARGET_FILE:hevc-decode> yuv444p)
    set_tests_properties(oracle_i_p_qcif_444_tskip_4f PROPERTIES LABELS "oracle;feature"
        SKIP_RETURN_CODE 2)

    # Phase 5 — P-frames, no filters
    add_test(NAME oracle_p_qcif_10f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/p_qcif_10f.265 176 144
            921650ab84b8e366a1aa4e16f2527e46 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_p_qcif_10f PROPERTIES LABELS "oracle;phase5"
        SKIP_RETURN_CODE 2)

    # Phase 5 — B-frames, no filters
    add_test(NAME oracle_b_qcif_10f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/b_qcif_10f.265 176 144
            dffe2712c0561c75ee40256d59299826 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_b_qcif_10f PROPERTIES LABELS "oracle;phase5"
        SKIP_RETURN_CODE 2)

    # Phase 6 — I-frame with deblocking only
    add_test(NAME oracle_i_64x64_deblock
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_deblock.265 64 64
            576ef497ceedffa01709ddfaa13c76c3 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_i_64x64_deblock PROPERTIES LABELS "oracle;phase6"
        SKIP_RETURN_CODE 2)

    # Phase 6 — I-frame with SAO only
    add_test(NAME oracle_i_64x64_sao
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_sao.265 64 64
            eacb4952591daa891e8dc5344c6c34ee $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_i_64x64_sao PROPERTIES LABELS "oracle;phase6"
        SKIP_RETURN_CODE 2)

    # Phase 6 — I-frame with deblocking + SAO
    add_test(NAME oracle_i_64x64_full
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_full.265 64 64
            dcb7ae02b4514b6e7fd801ae691d901a $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_i_64x64_full PROPERTIES LABELS "oracle;phase6"
        SKIP_RETURN_CODE 2)

    # Phase 6 — Full pipeline (B-frames + deblocking + SAO) = Main profile milestone
    add_test(NAME oracle_full_qcif_10f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/full_qcif_10f.265 176 144
            a4984ee61d18ac9619808c5338132eaa $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_full_qcif_10f PROPERTIES LABELS "oracle;phase6;milestone"
        SKIP_RETURN_CODE 2)

    # Real-world bitstreams — Big Buck Bunny (Main profile, full pipeline)
    add_test(NAME oracle_bbb1080_50f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/bbb1080_50f.265 1920 1080
            a7c12e24701e3636ce58246033bff6ed $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_bbb1080_50f PROPERTIES LABELS "oracle;phase6;realworld"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_bbb4k_25f
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/bbb4k_25f.265 3840 2160
            69f2ba71153ec5f3c1573fed17631ed4 $<TARGET_FILE:hevc-decode>)
    set_tests_properties(oracle_bbb4k_25f PROPERTIES LABELS "oracle;phase6;realworld"
        SKIP_RETURN_CODE 2)

    # Phase 7 — 10-bit (Main 10 profile)
    add_test(NAME oracle_i_64x64_10bit
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/i_64x64_10bit.265 64 64
            230cf887beecf0dc1722a823f998de47 $<TARGET_FILE:hevc-decode> yuv420p10le)
    set_tests_properties(oracle_i_64x64_10bit PROPERTIES LABELS "oracle;phase7;main10"
        SKIP_RETURN_CODE 2)

    add_test(NAME oracle_full_qcif_10f_10bit
        COMMAND ${ORACLE_SCRIPT} ${FIXTURES_DIR}/full_qcif_10f_10bit.265 176 144
            19efbf36509c6ecb86e42a7f709998b5 $<TARGET_FILE:hevc-decode> yuv420p10le)
    set_tests_properties(oracle_full_qcif_10f_10bit PROPERTIES LABELS "oracle;phase7;main10;milestone"
        SKIP_RETURN_CODE 2)

    # Edge-case conformance tests (generated by tools/fetch_conformance.sh)
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/conformance/edge-cases/manifest.cmake")
        include("${CMAKE_SOURCE_DIR}/tests/conformance/edge-cases/manifest.cmake")
    endif()
endif()
