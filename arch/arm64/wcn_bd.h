/* arch/arm64/wcn_bd.h — дескрипторы буфера кадров (BD) прошивки Wi-Fi.
 *
 * Перед каждым кадром 802.11, уходящим или приходящим через DXE, лежит
 * дескриптор буфера: сорок с лишним байт полей, по которым прошивка
 * понимает, что за кадр и что с ним делать. Поля — битовые, и порядок
 * их важен до бита, поэтому структуры взяты дословно из txrx.h драйвера
 * wcn36xx (ISC). Собираются они в порядке хоста (LE), а перед отправкой
 * каждое 32-битное слово переворачивается в порядок железа — buff_to_be. */
#ifndef ARM64_WCN_BD_H
#define ARM64_WCN_BD_H

#include "wcn_hal.h"

struct wcn36xx_pdu {
	u32	dpu_fb:8;
	u32	adu_fb:8;
	u32	pdu_id:16;
	u32	tail_pdu_idx:16;
	u32	head_pdu_idx:16;
	u32	pdu_count:7;
	u32	mpdu_data_off:9;
	u32	mpdu_header_off:8;
	u32	mpdu_header_len:8;
	u32	reserved4:8;
	u32	tid:4;
	u32	bd_ssn:2;
	u32	reserved3:2;
	u32	mpdu_len:16;
} __packed;

struct wcn36xx_rx_bd {
	u32	bdt:2;
	u32	ft:1;
	u32	dpu_ne:1;
	u32	rx_key_id:3;
	u32	ub:1;
	u32	rmf:1;
	u32	uma_bypass:1;
	u32	csr11:1;
	u32	reserved0:1;
	u32	scan_learn:1;
	u32	rx_ch:4;
	u32	rtsf:1;
	u32	bsf:1;
	u32	a2hf:1;
	u32	st_auf:1;
	u32	dpu_sign:3;
	u32	dpu_rf:8;
	struct wcn36xx_pdu pdu;
	u32	addr3:8;
	u32	addr2:8;
	u32	addr1:8;
	u32	dpu_desc_idx:8;
	u32	rxp_flags:23;
	u32	rate_id:9;
	u32	phy_stat0;
	u32	phy_stat1;
	u32	rx_times;
	u32	pmi_cmd[6];
	u32	reserved7:4;
	u32	reorder_slot_id:6;
	u32	reorder_fwd_id:6;
	u32	reserved6:12;
	u32	reorder_code:4;
	u32	exp_seq_num:12;
	u32	cur_seq_num:12;
	u32	rf_band:2;
	u32	fr_type_subtype:6;
	u32	msdu_size:16;
	u32	sub_fr_id:4;
	u32	proc_order:4;
	u32	reserved9:4;
	u32	aef:1;
	u32	lsf:1;
	u32	esf:1;
	u32	asf:1;
} __packed;

struct wcn36xx_tx_bd {
	u32	bdt:2;
	u32	ft:1;
	u32	dpu_ne:1;
	u32	fw_tx_comp:1;
	u32	tx_comp:1;
	u32	reserved1:1;
	u32	ub:1;
	u32	rmf:1;
	u32	reserved0:12;
	u32	dpu_sign:3;
	u32	dpu_rf:8;
	struct wcn36xx_pdu pdu;
	u32	reserved5:7;
	u32	queue_id:5;
	u32	bd_rate:2;
	u32	ack_policy:2;
	u32	sta_index:8;
	u32	dpu_desc_idx:8;
	u32	tx_bd_sign;
	u32	reserved6;
	u32	dxe_start_time;
	u32	dxe_end_time;
} __packed;

/* Перевернуть каждое 32-битное слово (buff_to_be из драйвера). */
static inline void wcn_buff_to_be(void *buf, unsigned words) {
	u32 *p = (u32 *)buf;
	for (unsigned i = 0; i < words; i++) {
		u32 v = p[i];
		p[i] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
	}
}

#endif
