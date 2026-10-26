/*
 * Load and store the whole ESP32-S3 TIE state at a2 (ee.h): q0..q7
 * through the vector loads, QACC, ACCX, SAR_BYTE, FFT_BIT_WIDTH, GPIO_OUT and
 * UA_STATE through their user registers, SAR through WSR/RSR and f0..f15
 * through the FP loads. The generated cases call these with call8 around
 * the one instruction they test.
 */
    .text

    .global ee_load_state
    .type   ee_load_state, @function
    .align  4
ee_load_state:
    entry   a1, 16
    mov     a3, a2
    ee.vld.128.ip   q0, a3, 16
    ee.vld.128.ip   q1, a3, 16
    ee.vld.128.ip   q2, a3, 16
    ee.vld.128.ip   q3, a3, 16
    ee.vld.128.ip   q4, a3, 16
    ee.vld.128.ip   q5, a3, 16
    ee.vld.128.ip   q6, a3, 16
    ee.vld.128.ip   q7, a3, 16
    l32i    a4, a2, 128
    wur.qacc_l_0    a4
    l32i    a4, a2, 132
    wur.qacc_l_1    a4
    l32i    a4, a2, 136
    wur.qacc_l_2    a4
    l32i    a4, a2, 140
    wur.qacc_l_3    a4
    l32i    a4, a2, 144
    wur.qacc_l_4    a4
    l32i    a4, a2, 148
    wur.qacc_h_0    a4
    l32i    a4, a2, 152
    wur.qacc_h_1    a4
    l32i    a4, a2, 156
    wur.qacc_h_2    a4
    l32i    a4, a2, 160
    wur.qacc_h_3    a4
    l32i    a4, a2, 164
    wur.qacc_h_4    a4
    l32i    a4, a2, 168
    wur.accx_0      a4
    l32i    a4, a2, 172
    wur.accx_1      a4
    l32i    a4, a2, 176
    wsr.sar         a4
    l32i    a4, a2, 180
    wur.sar_byte    a4
    l32i    a4, a2, 184
    wur.fft_bit_width   a4
    l32i    a4, a2, 188
    wur.gpio_out    a4
    l32i    a4, a2, 192
    wur.ua_state_0  a4
    l32i    a4, a2, 196
    wur.ua_state_1  a4
    l32i    a4, a2, 200
    wur.ua_state_2  a4
    l32i    a4, a2, 204
    wur.ua_state_3  a4
    lsi     f0, a2, 208
    lsi     f1, a2, 212
    lsi     f2, a2, 216
    lsi     f3, a2, 220
    lsi     f4, a2, 224
    lsi     f5, a2, 228
    lsi     f6, a2, 232
    lsi     f7, a2, 236
    lsi     f8, a2, 240
    lsi     f9, a2, 244
    lsi     f10, a2, 248
    lsi     f11, a2, 252
    lsi     f12, a2, 256
    lsi     f13, a2, 260
    lsi     f14, a2, 264
    lsi     f15, a2, 268
    retw

    .global ee_store_state
    .type   ee_store_state, @function
    .align  4
ee_store_state:
    entry   a1, 16
    mov     a3, a2
    ee.vst.128.ip   q0, a3, 16
    ee.vst.128.ip   q1, a3, 16
    ee.vst.128.ip   q2, a3, 16
    ee.vst.128.ip   q3, a3, 16
    ee.vst.128.ip   q4, a3, 16
    ee.vst.128.ip   q5, a3, 16
    ee.vst.128.ip   q6, a3, 16
    ee.vst.128.ip   q7, a3, 16
    rur.qacc_l_0    a4
    s32i    a4, a2, 128
    rur.qacc_l_1    a4
    s32i    a4, a2, 132
    rur.qacc_l_2    a4
    s32i    a4, a2, 136
    rur.qacc_l_3    a4
    s32i    a4, a2, 140
    rur.qacc_l_4    a4
    s32i    a4, a2, 144
    rur.qacc_h_0    a4
    s32i    a4, a2, 148
    rur.qacc_h_1    a4
    s32i    a4, a2, 152
    rur.qacc_h_2    a4
    s32i    a4, a2, 156
    rur.qacc_h_3    a4
    s32i    a4, a2, 160
    rur.qacc_h_4    a4
    s32i    a4, a2, 164
    rur.accx_0      a4
    s32i    a4, a2, 168
    rur.accx_1      a4
    s32i    a4, a2, 172
    rsr.sar         a4
    s32i    a4, a2, 176
    rur.sar_byte    a4
    s32i    a4, a2, 180
    rur.fft_bit_width   a4
    s32i    a4, a2, 184
    rur.gpio_out    a4
    s32i    a4, a2, 188
    rur.ua_state_0  a4
    s32i    a4, a2, 192
    rur.ua_state_1  a4
    s32i    a4, a2, 196
    rur.ua_state_2  a4
    s32i    a4, a2, 200
    rur.ua_state_3  a4
    s32i    a4, a2, 204
    ssi     f0, a2, 208
    ssi     f1, a2, 212
    ssi     f2, a2, 216
    ssi     f3, a2, 220
    ssi     f4, a2, 224
    ssi     f5, a2, 228
    ssi     f6, a2, 232
    ssi     f7, a2, 236
    ssi     f8, a2, 240
    ssi     f9, a2, 244
    ssi     f10, a2, 248
    ssi     f11, a2, 252
    ssi     f12, a2, 256
    ssi     f13, a2, 260
    ssi     f14, a2, 264
    ssi     f15, a2, 268
    retw
