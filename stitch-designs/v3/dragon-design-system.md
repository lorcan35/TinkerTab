# Design System Strategy: The Monolithic Interface

## 1. Overview & Creative North Star
The Creative North Star for this design system is **"The Digital Monolith."** 

This system moves away from the "app-on-a-phone" aesthetic toward a singular, integrated physical-to-digital object. Inspired by high-end horology and brutalist architecture, the interface is treated not as a series of screens, but as a continuous, carved surface of "warm obsidian." 

We break the "template" look through **Intentional Asymmetry.** While the 8px grid is our foundation, we use it to create off-center focal points—placing heavy display type against vast negative space to create a sense of premium, editorial breathing room. The goal is a high-tech, disciplined atmosphere where every pixel of `primary` Electric Cyan feels earned and significant.

---

## 2. Color & Surface Architecture
We do not use color to decorate; we use it to architect information. The palette is a "Linear Warm Dark" range that avoids the sterility of pure black.

### The "No-Line" Rule
Standard 1px borders are strictly prohibited for sectioning. Structural boundaries must be defined solely through background shifts.
*   **Implementation:** Use `surface-container-low` for secondary information blocks sitting on the `surface` background. 
*   **The Intent:** This creates a "molded" look, as if the UI components were machined from the same block of material.

### Surface Hierarchy & Nesting
Depth is achieved via the "Tonal Layering" principle. Treat the UI as stacked sheets of tinted glass:
*   **Base Layer:** `background` (#0c0e11)
*   **Primary Content Area:** `surface` (#0c0e11)
*   **Nested Components:** `surface-container` (#161a1f)
*   **Interactive Overlays:** `surface-container-highest` (#20262f)

### The "Glass & Soul" Rule
To prevent the UI from feeling flat, utilize `backdrop-blur` (12px–20px) on floating elements using semi-transparent versions of `surface-bright`.
*   **Signature Texture:** Use a subtle linear gradient on primary CTAs transitioning from `primary` (#00daf3) to `primary-container` (#004f58). This adds a "lithographic glow" that flat fills lack.

---

## 3. Typography: Editorial Authority
The contrast between the technical precision of **Space Grotesk** and the neutral clarity of **Inter** creates a "Laboratory-Manual-meets-Vogue" aesthetic.

*   **Display & Headlines (Space Grotesk):** Use `display-lg` and `headline-lg` with tight tracking (-2%) for high-impact AI responses or device states. These should often be left-aligned with significant bottom padding (32px+) to create an asymmetric, editorial anchor.
*   **Body & Labels (Inter):** Reserved for data, settings, and secondary descriptions. `body-md` is your workhorse.
*   **Hierarchy Note:** Use `on-surface-variant` (Text-secondary) for metadata to ensure the `on-surface` (Text-primary) "pops" with high-contrast authority.

---

## 4. Elevation & Depth: Tonal Physics
Traditional drop shadows are replaced by **Ambient Luminance.**

*   **The Layering Principle:** To lift a card, move it up one tier in the `surface-container` scale. A `surface-container-high` card placed on a `surface-container` background provides a sophisticated, "soft" lift.
*   **The Ghost Border:** For high-density data areas where separation is critical, use the `outline-variant` token at 15% opacity. It should be felt, not seen.
*   **Atmospheric Glow:** When the AI is active, instead of a shadow, apply a very large, low-opacity (4%) radial gradient of `primary` behind the active container to simulate the device "powering up."

---

## 5. Components & Primitives

### Buttons: The Kinetic Trigger
*   **Primary:** Solid `primary` background. Type is `on-primary` (Dark), bold Inter. 12px radius.
*   **Secondary:** Ghost style. No fill. `outline` border at 20% opacity. `primary` text.
*   **Tertiary:** Text only. `on-surface-variant` with a `primary` underline (2px) only on active state.

### Input Fields: The Data Slot
*   **Styling:** Forgo the box. Use a `surface-container-low` background with a 2px bottom stroke of `outline-variant`. 
*   **Active State:** The bottom stroke transitions to `primary` (Electric Cyan).
*   **Error:** Background shifts to a subtle 5% tint of `error`.

### Cards & Lists: Seamless Flow
*   **Constraint:** Zero dividers. 
*   **Separation:** Use `spacing-6` (1.5rem) of vertical white space or a subtle shift from `surface` to `surface-container-low`. 
*   **Interactive Lists:** On press, the background should flash to `surface-bright` briefly.

### Signature Component: The "Glyph Pulse"
An AI-specific component. A high-contrast circular element using `display-lg` Space Grotesk numerals or symbols, centered in a `surface-container-highest` ring, used to represent processing states.

---

## 6. Do’s and Don’ts

### Do
*   **DO** use extreme typographic scale. A `display-lg` header next to a `label-sm` creates a premium, custom feel.
*   **DO** embrace the "Warm Dark" tones. Ensure `surface` colors have that slight hint of blue/grey to maintain the "high-tech" atmosphere.
*   **DO** prioritize the 48px touch target, even if the visual element (like a small icon) is only 24px.

### Don't
*   **DON'T** use 100% white. Always use `on-surface` (#e0e6f1) to prevent eye strain on the 720x1280 display.
*   **DON'T** use the `Success`, `Warning`, or `Error` colors for decoration. They are functional "status" signals only.
*   **DON'T** center-align long blocks of text. Stick to the disciplined, left-aligned grid to maintain the "OS" feel.
*   **DON'T** use shadows. If it needs to stand out, make it lighter or give it a `primary` glow.