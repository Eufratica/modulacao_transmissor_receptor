#ifndef QAM_DEMOD_H
#define QAM_DEMOD_H

#include <stdbool.h>
#include <stdint.h>
#include "rx_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DEMODULADOR 4-QAM EM QUADRATURA COM COSTAS LOOP E RECUPERAÇÃO GARDNER
 * ============================================================================
 * Pipeline por amostra:
 *   1. Estimador/removedor de offset DC adaptativo + AGC por envoltória RMS.
 *   2. Misturador em quadratura (cos e +sin/-sin) guiado por NCO de fase contínua.
 *   3. Filtro passa-baixas boxcar de 1 ciclo de portadora (zero exato em 2*fc).
 *   4. Costas Loop PI de 2ª ordem para recuperação de fase e desvio de frequência.
 *   5. Detector de erro de temporização Gardner + integrador no platô central.
 * ============================================================================ */

#define QAM_DEMOD_MAX_BOXCAR 128U

typedef struct {
    bool  symbol_ready;     /**< true na amostra em que um novo símbolo foi integrado. */
    bool  burst_started;    /**< true quando a energia ultrapassa o limiar de squelch. */
    bool  burst_ended;      /**< true quando o canal volta ao silêncio após uma rajada. */
    float i_hat;            /**< Estimativa normalizada do canal I no símbolo. */
    float q_hat;            /**< Estimativa normalizada do canal Q no símbolo. */
    float snr_est_db;       /**< Estimativa aproximada da SNR do símbolo em dB. */
} qam_demod_out_t;

typedef struct {
    qam_rx_config_t cfg;

    /* Estado de pré-processamento (DC, potência e squelch) */
    bool     dc_primed;
    float    dc_est_mv;
    float    pwr_ema;
    float    amp_pk_est_mv;
    bool     signal_active;
    uint32_t active_cnt;
    uint32_t quiet_cnt;
    uint32_t quiet_limit_samples;

    /* NCO e Costas Loop (fase e frequência da portadora) */
    float    nco_phase_rad;
    float    nco_omega0_rad;
    float    costas_freq_rad;
    float    costas_freq_max_rad;
    uint32_t costas_decim_cnt;

    /* Filtro passa-baixas boxcar (comprimento = round(fs / fc)) */
    float    box_i[QAM_DEMOD_MAX_BOXCAR];
    float    box_q[QAM_DEMOD_MAX_BOXCAR];
    float    box_sum_i;
    float    box_sum_q;
    uint32_t box_len;
    uint32_t box_idx;

    /* Recuperação de temporização de símbolo (Gardner TED) + integrador */
    float    sym_phase;
    float    sym_inc_nom;
    float    sym_rate_corr;
    float    mid_i_curr;
    float    mid_q_curr;
    float    mid_i_prev;
    float    mid_q_prev;
    bool     has_prev_mid;
    float    bnd_i_curr;
    float    bnd_q_curr;
    bool     has_curr_bnd;
    float    acc_i;
    float    acc_q;
    uint32_t acc_cnt;

    /* Métricas de monitoramento */
    float    last_i_hat;
    float    last_q_hat;
    float    last_ped;
    float    last_gardner_err;
    float    freq_offset_hz;
    uint32_t total_symbols;
} qam_demod_t;

/**
 * @brief Inicializa o demodulador 4-QAM e prepara a LUT senoidal interna.
 */
bool qam_demod_init(qam_demod_t *demod, const qam_rx_config_t *cfg);

/**
 * @brief Atualiza parâmetros mantendo ou reiniciando as malhas conforme necessário.
 */
bool qam_demod_reconfigure(qam_demod_t *demod, const qam_rx_config_t *cfg);

/**
 * @brief Reinicia o estado das malhas de fase, frequência e temporização para nova rajada.
 */
void qam_demod_reset_tracking(qam_demod_t *demod);

/**
 * @brief Processa uma amostra em milivolts vinda do ADC (ou simulação).
 */
void qam_demod_process_sample(qam_demod_t *demod, float sample_mv, qam_demod_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* QAM_DEMOD_H */
