; group: 1,1,1
; Read the eight application-initialized words at byte 128 through VCC as an ordinary pointer pair.
s_mov_b32 s4, 128
s_mov_b32 s5, 0
s_mov_b64 vcc, s[4:5]
s_load_dwordx8 s[8:15], vcc, 0
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, 0
v_mov_b32 v1, s8
v_mov_b32 v2, s9
v_mov_b32 v3, s10
v_mov_b32 v4, s11
buffer_store_dwordx4 v[1:4], v0, s[0:3], 0 offen
v_mov_b32 v0, 16
v_mov_b32 v1, s12
v_mov_b32 v2, s13
v_mov_b32 v3, s14
v_mov_b32 v4, s15
buffer_store_dwordx4 v[1:4], v0, s[0:3], 0 offen
s_endpgm
