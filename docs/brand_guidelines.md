# ZeptoDB Brand Guidelines & Design System (Electric Indigo)

ZeptoDB is an ultra-low latency in-memory database designed for High-Frequency Trading (HFT) and financial markets. This design system ensures that the power and reliability of the engine are reflected in a high-performance, production-grade Web Console UI.

---

## 1. Brand Concept
**"Precision of Light, Weight of Data"**

The ZeptoDB Console is more than just a management tool; it is a "Command & Control" center for engineers. By moving from neon tones to a deeper **Electric Indigo**, we evoke a sense of professional trust and technical sophistication required in the financial sector.

* **Reliability:** Deep indigo tones represent the stability of the system.
* **Dynamics:** High-contrast Teal Mint accents visualize the flow of real-time data.
* **Readability:** Precision-engineered contrast for long-term monitoring with minimal eye strain.

---

## 2. Color System
A production-ready palette inspired by Material Design 3 (Material You) tonal structures.

### 🔵 Primary - Trust & Intelligence
| Level | Hex Code | Usage |
| :--- | :--- | :--- |
| **Main** | `#4D7CFF` | **Electric Indigo**: Primary buttons, active states, key metrics. |
| **Light** | `#8EACFF` | Hover states and secondary highlights. |
| **Dark** | `#2A45B3` | Pressed states and high-emphasis text. |

### 🟢 Secondary - Vitality & Speed
| Level | Hex Code | Usage |
| :--- | :--- | :--- |
| **Main** | `#00F5D4` | **Teal Mint**: Success indicators, data growth, positive trends. |
| **Light** | `#66FFE6` | Subtle accents and soft UI feedback. |
| **Dark** | `#00BFA5` | Secondary data points in charts. |

### ⚫ Background & Surface
* **Background Default:** `#0A0C10` (Deep Obsidian) – Ultra-low luminosity cool-toned black.
* **Background Paper:** `#11161D` (Carbon Dark) – Base layer for cards, sidebars, and panels.
* **Surface Elevation:** `#1B2129` – Top-level surfaces like popovers and modals.

### 🚦 Status Colors
* **Success:** `#00E676` (Mint Green)
* **Warning:** `#FFB300` (Amber) – Enhanced visibility for latency warnings.
* **Error:** `#FF1744` (Laser Red) – Immediate attention required (e.g., disconnection).
* **Info:** `#2979FF` (Azure Blue) – General system notifications.

---

## 3. Typography
A dual-font system designed for both interface clarity and code readability.

* **Primary Font:** `Inter`
    * **Usage:** Headings, Body, UI elements (Buttons, Labels).
    * **Features:** Modern sans-serif with excellent legibility at small sizes.
* **Monospace Font:** `JetBrains Mono`
    * **Usage:** Code snippets, Query editors, Logs, Timestamps, and Numerical data.
    * **Features:** Optimized for developers with clear character distinction.

---

## 4. UI Components & Shape
* **Border Radius:** `8px` – Sharp, disciplined corners reflecting precise engineering.
* **Borders:** `1px solid rgba(255, 255, 255, 0.08)` – Used to define depth in dark mode.
* **Glow Effect (Primary):** `drop-shadow(0 0 8px rgba(77, 124, 255, 0.3))` – Applied to active indicators to give a subtle "powered-on" physical feel.
* **Micro-animations:** Transitions capped at **0.2s** to maintain the "Ultra-low Latency" feel throughout the user experience.

---

## 5. Logo Guidelines
Until a physical logo asset is created, use the following typographic treatment:

* **Style:** `Zepto` (**Bold 700**, Electric Indigo) + `DB` (**Light 300**, White/Slate)
* **Markup Example:** ```html
    <span style="color: #4D7CFF; font-weight: 700;">Zepto</span>
    <span style="color: #E2E8F0; font-weight: 300;">DB</span>
    ```
* **Concept:** The primary color on "Zepto" emphasizes innovation, while the neutral "DB" emphasizes the solid foundation of the database.