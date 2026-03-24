// ============================================================================
// Phase C-1: UCXBackend 구현 파일
// ============================================================================
#include "ucx_backend.h"
#include <stdexcept>

namespace zeptodb::cluster {

#ifdef ZEPTO_HAS_UCX
// 명시적 인스턴스화
template class TransportBackend<UCXBackend>;
#endif

} // namespace zeptodb::cluster
