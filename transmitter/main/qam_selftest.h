#ifndef QAM_SELFTEST_H
#define QAM_SELFTEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * SUÍTE DE AUTO-TESTE EM SOFTWARE DO TRANSMISSOR 4-QAM
 * ============================================================================
 * Valida matematicamente mapeamento, conversão ASCII->símbolos, enquadramento,
 * CRC-16, geração da portadora, fase/amplitude dos 4 símbolos por DFT em
 * quadratura e limites elétricos de segurança.
 * ============================================================================ */

/**
 * @brief Executa todos os testes automatizados de software e imprime o relatório.
 *
 * @param verbose true para detalhar valores numéricos de cada verificação.
 * @return true se todos os testes passaram; false se houver qualquer falha.
 */
bool qam_selftest_run_all(bool verbose);

#ifdef __cplusplus
}
#endif

#endif /* QAM_SELFTEST_H */
