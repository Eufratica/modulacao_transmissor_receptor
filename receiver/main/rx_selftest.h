#ifndef RX_SELFTEST_H
#define RX_SELFTEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * AUTO-TESTE EM MALHA FECHADA DO RECEPTOR 4-QAM
 * ============================================================================
 * Sintetiza um quadro Modo 2 completo com erro de fase arbitrária, desvio de
 * frequência de portadora, deslocamento de amostragem de símbolo, offset DC e
 * atenuação, alimenta o demodulador/desenquadrador reais e valida a recuperação
 * integral da mensagem com CRC-16 válido.
 * ============================================================================ */

bool rx_selftest_run_all(bool verbose);

#ifdef __cplusplus
}
#endif

#endif /* RX_SELFTEST_H */
