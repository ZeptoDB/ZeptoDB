// ============================================================================
// Phase C-1: SharedMemBackend 구현 파일
// ============================================================================
// 헤더 전용 구현이라 실제 로직은 shm_backend.h에 있음
// 이 파일은 명시적 인스턴스화 및 빌드 시스템 통합을 위해 존재
// ============================================================================

#include "zeptodb/cluster/transport.h"
#include "shm_backend.h"

namespace zeptodb::cluster {

// SharedMemBackend는 헤더에서 완전히 구현됨 (CRTP 템플릿)
// 명시적 인스턴스화 (링크 타임 최적화)
template class TransportBackend<SharedMemBackend>;

} // namespace zeptodb::cluster
