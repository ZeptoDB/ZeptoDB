# ZeptoDB 브랜드 가이드라인 및 디자인 시스템 (Brand Guidelines & Design System)

ZeptoDB는 HFT(고빈도 매매) 및 금융 시장을 타겟으로 하는 초저지연(Ultra-low latency) 인메모리 데이터베이스입니다. 이 강력한 성능과 신뢰성을 웹 콘솔 UI에도 동일하게 투영하기 위해, 시각적으로 압도적이면서도 직관적인 프로덕션 레벨의 디자인 시스템을 구축합니다.

## 1. 브랜드 컨셉 (Brand Concept)
**"엔지니어링의 정점, 타협 없는 속도와 신뢰 (Peak Engineering, Uncompromising Speed and Trust)"**

ZeptoDB 콘솔은 개발자와 금융 엔지니어가 장시간 모니터링하고 제어해도 눈의 피로가 적은 **고급스러운 다크 모드(Premium Dark Mode)**를 기본으로 합니다. 빛의 속도로 데이터를 처리하는 데이터베이스의 특성을 반영하여, 네온 포인트 컬러와 솔리드한 다크 배경의 대비를 통해 미래지향적이고 전문적인 느낌을 줍니다.

- **전문성 (Professionalism):** 불필요한 장식을 배제하고 데이터 가독성을 극대화합니다.
- **초저지연 (Ultra-low Latency):** 빠르고 경쾌한 마이크로 인터랙션과 시각적 피드백을 제공합니다.
- **신뢰성 (Reliability):** 안정감을 주는 깊은 톤의 배경과 명확한 대비를 가진 타이포그래피를 사용합니다.

---

## 2. 컬러 시스템 (Color System)
최고급 프로덕션 수준의 세련된 다크 테마 컬러 팔레트입니다.

### 🔴 Primary (주조색) - 초고속 & 기술력
- **Main:** `#00E5FF` (Neon Cyan) - 데이터를 처리하는 초고속 프로세스의 빛을 시각화.
- **Light:** `#69FFFF`
- **Dark:** `#00B2CC`
- **활용:** 주요 버튼, 포커스 상태, 중요 활성 지표 등.

### 🟣 Secondary (보조색) - 고급스러움 & 직관력
- **Main:** `#7C4DFF` (Electric Purple) - 데이터베이스의 깊이감과 고급스러운 브랜드 아이덴티티.
- **Light:** `#B47CFF`
- **Dark:** `#3F1DCC`
- **활용:** 보조 액션, 선택적 하이라이트, 차트/그래프 데이터 포인트 등.

### ⚫ Background & Surface (배경 및 표면) - 눈의 피로를 낮추는 깊이감
- **Background Default:** `#0A0C10` (Deep Obsidian) - 완전한 검은색이 아닌 아주 깊은 쿨톤 블랙으로 눈의 피로 감소.
- **Background Paper (Surface):** `#11161D` (Carbon Dark) - 카드, 모달, 사이드바 등 패널의 기본 배경.
- **Surface Elevation:** 뎁스(Depth)에 따라 밝은 톤의 보더(Border)나 은은한 그림자(Glow) 효과를 적용.

### 🟢 Status Colors (상태 색상)
- **Success:** `#00E676` (Mint Green) - 정상 동작 및 완료된 프로세스.
- **Warning:** `#FFEA00` (Cyber Yellow) - 주의 필요, 지연 발생 징후.
- **Error:** `#FF1744` (Laser Red) - 치명적 오류, 연결 끊김.
- **Info:** `#2979FF` (Azure Blue) - 일반 정보 및 공지.

---

## 3. 타이포그래피 (Typography)
명확한 정보 전달 및 코드 가독성을 위한 듀얼 폰트 시스템을 사용합니다.

- **Primary Font:** `Inter`
  - 용도: 제목(Headings), 본문(Body), UI 요소(버튼, 캡션 등).
  - 특징: 글자 폭이 균일하고 가독성이 매우 뛰어난 모던 산세리프 폰트.
- **Monospace Font:** `JetBrains Mono`
  - 용도: 코드 스니펫, 에디터 영역, 데이터베이스 쿼리, 로그 및 타임스탬프, 수치형 데이터 표시.
  - 특징: 개발자 친화적인 폰트로 뛰어난 식별력.

---

## 4. UI 컴포넌트 & 쉐이프 (Shape & Components)

- **Border Radius (모서리 둥기):** `8px` - 너무 둥글지 않은 샤프한 모서리를 통해 단단하고 정밀한 엔지니어링의 느낌을 전달합니다.
- **Buttons (버튼):**
  - Contained Button: 약간의 그라디언트 혹은 플랫한 배경에 Hover 시 매끄러운 밝기 증가(마이크로 애니메이션) 적용.
  - Outlined Button: 은은한 투명도 배경과 샤프한 보더라인 적용.
- **Cards & Layout Panels:**
  - 카드는 내부 컨텐츠 그룹을 구분하기 위해 1픽셀 반투명 보더(`rgba(255, 255, 255, 0.08)`)를 가져 입체감을 줌.
- **Data Tables:**
  - 호버 시 약간의 배경색 변화로 행 구분을 명확히 하며, 헤더는 바탕색보다 약간 밝은 톤을 적용하여 강한 구분을 줌.
- **Micro-animations:** 요소에 마우스를 올릴 때(Hover), 클릭할 때(Ripple) 빠른 전환 시간(0.2s 이하)을 두어 초저지연 애플리케이션의 빠릿빠릿한 조작감을 UX 적으로도 제공합니다.

---

## 5. 로고 가이드 (Logo Guidelines)
아직 물리적인 이미지 에셋이 없다면 타이포그래피 로고를 우선 사용합니다.
- **형태:** `Zepto` (Primary Color) + `DB` (White 또는 텍스트 기본 색상)
- **폰트웨이트:** `Zepto`는 **Bold (700)**, `DB`는 **Regular (400)** 또는 그 반대로 하여 대비를 줍니다.
- **예시:** `<span style="color: #00E5FF; font-weight: 700;">Zepto</span><span style="font-weight: 300;">DB</span>`

이 브랜드 가이드라인을 바탕으로 Material UI 테마를 전면 개편하여 `web/` 폴더에 프로덕션 레벨의 UI를 반영할 것입니다.
