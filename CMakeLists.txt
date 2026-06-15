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
    # Enable exceptions + SIMD 128-bit for the whole project in WASM mode.
    # -msse2 lets emscripten lower our portable SSE2 kernels (<emmintrin.h>) to
    # wasm128 — the same intrinsics compile natively on x86-64, so the oracle
    # tests verify the exact code that ships to WASM.
    target_compile_options(hevc-decode-lib PRIVATE -fexceptions -msimd128 -msse2)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(hevc-wasm PRIVATE -O0 -g -fexceptions -msimd128)
        # Debug: keep emscripten runtime sanity checks (stack cookies, bounds).
        target_link_options(hevc-wasm PRIVATE "SHELL:-s ASSERTIONS=1")
    else()
        # Release: drop emscripten runtime sanity checks (ASSERTIONS adds stack
        # cookie / bounds checks that cost real cycles in the hot decode loop).
        # Bit-exact (verified vs oracle); ~3% faster decode at 1080p and 4K.
        # NB: -flto was measured here and REGRESSED decode ~3% (emcc's cross-TU
        # inlining bloats the hot path / hurts I-cache), so it is intentionally off.
        target_compile_options(hevc-wasm PRIVATE -O3 -fexceptions -msimd128)
        target_link_options(hevc-wasm PRIVATE "SHELL:-s ASSERTIONS=0")
    endif()
    target_link_options(hevc-wasm PRIVATE
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=HEVCDecoderModule"
        "SHELL:-s STACK_SIZE=4194304"
        "SHELL:-s NO_DISABLE_EXCEPTION_CATCHING=1"
        "SHELL:-s EXPORTED_FUNCTIONS=[_hevc_decoder_create,_hevc_decoder_destroy,_hevc_decoder_decode,_hevc_decoder_get_frame_count,_hevc_decoder_get_frame,_hevc_decoder_get_info,_hevc_decoder_get_recovery_point,_hevc_decoder_feed,_hevc_decoder_drain,_hevc_decoder_get_drained_frame,_hevc_decoder_flush,_hevc_decoder_reset,_malloc,_free]"
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
    )

    target_link_libraries(hevc_tests PRIVATE
        hevc-decode-lib
        GTest::gtest_main
    )

    target_compile_definitions(hevc_tests PRIVATE
        FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/conformance/fixtures"
    )

    include(GoogleTest)
    gtest_discover_tests(hevc_tests)

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
