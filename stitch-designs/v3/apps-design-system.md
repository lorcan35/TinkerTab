# Design System Specification: The Kinetic Monolith

## 1. Overview & Creative North Star
The Creative North Star for this design system is **"The Kinetic Monolith."** It is an aesthetic rooted in high-precision utility, moving away from the "app-like" clutter of modern smartphones toward a dedicated, AI-driven hardware feel. 

By marrying the monochrome discipline of high-end industrial design with the "warm dark" depth of professional engineering tools, we create an interface that feels less like a website and more like a tactical OS. We break the "template" look by utilizing extreme typographic scale contrasts and intentional asymmetry—using whitespace as a functional element rather than a void.

---

## 2. Colors & Surface Architecture
The palette is a sophisticated range of "Obsidian Tones." We use color not to decorate, but to define the physical architecture of the software.

### Palette Tokens (Material Convention)
*   **Background:** `#111317` (The void; used for the deepest layer)
*   **Surface:** `#111317` (The base plane)
*   **Surface-Container-Low:** `#1A1C1F`
*   **Surface-Container-High:** `#282A2D`
*   **Primary (Accent):** `#C3F5FF` (Electric Cyan - used for high-signal data)
*   **Primary-Container:** `#00E5FF` (The "Glow" state)
*   **On-Surface:** `#E2E2E6` (High-readability text)
*   **On-Surface-Variant:** `#BAC9CC` (Secondary metadata)

### The "No-Line" Rule
Standard 1px solid borders are strictly prohibited for sectioning. Boundaries must be defined through **Background Color Shifts**. For example, a card (Surface-Container-High) sits directly on the background (Surface-Dim) without a stroke. The 12px radius and the tonal shift provide all the separation required.

### Glassmorphism & Signature Textures
To achieve a "High-Tech AI" feel, use **Glassmorphism** for persistent overlays (like navigation bars or floating action buttons). 
*   **Glass State:** `surface-container-highest` at 60% opacity with a `24px` backdrop-blur.
*   **Signature Gradient:** For primary CTAs, use a linear gradient from `primary` (#C3F5FF) to `primary-container` (#00E5FF) at a 135-degree angle to simulate a glowing light-pipe effect.

---

## 3. Typography
We use a dual-typeface system to balance technical precision with human-centric readability.

*   **Display & Headlines (Space Grotesk):** This is our "Industrial" voice. It features wide apertures and geometric forms. Use `display-lg` (3.5rem) for hero moments and data visualization to create a bold, editorial impact.
*   **Body & Labels (Inter):** This is our "Functional" voice. Inter’s tall x-height ensures clarity on the 720x1280 screen at small sizes.

**The Hierarchy Rule:** Always pair a `headline-sm` in Space Grotesk with a `label-md` in Inter. The contrast between the geometric display font and the neutral body font creates a "signature" tech aesthetic.

---

## 4. Elevation & Depth
In this system, we do not use shadows. We use **Tonal Layering** to define the Z-axis.

*   **The Layering Principle:** Depth is achieved by "stacking." 
    *   Level 0: `surface-dim` (Background)
    *   Level 1: `surface-container-low` (Nested modules)
    *   Level 2: `surface-container-high` (Active cards/interactables)
*   **The "Ghost Border" Fallback:** If a component requires an edge for accessibility (e.g., an input field), use the `outline-variant` token (#3B494C) at **20% opacity**. It should feel like a faint etched line in glass, not a solid stroke.
*   **Interactive Glow:** Instead of a shadow, an active state may use a subtle outer glow using the `primary-fixed-dim` token at 10% opacity, simulating light emanating from the screen.

---

## 5. Components

### Buttons
*   **Primary:** Gradient fill (Primary to Primary-Container), Space Grotesk Bold, All-caps, 12px radius. Height: 48px minimum.
*   **Secondary:** `surface-container-highest` fill, no border, Inter Medium text.
*   **Tertiary:** Ghost style. No fill, no border. `on-surface-variant` text.

### Inputs
*   **Field:** `surface-container-low` fill. No bottom line. 12px border radius.
*   **Focus State:** The "Ghost Border" becomes 100% opaque `primary`.

### Cards & Lists
*   **The Divider Prohibition:** Forbid the use of 1px horizontal dividers. Use a `1.75rem` (Space 8) vertical gap between items or alternate background tones (`surface-container-lowest` vs `surface-container-low`).
*   **Interactive List Item:** On press, the background shifts to `surface-bright`.

### Signature Component: The "Glint" Status
*   A small 4px circular dot using the `primary` token, paired with a `label-sm` text. Used for "System Live" or "AI Processing" states.

---

## 6. Do's and Don'ts

### Do
*   **Do** use asymmetrical layouts. Align a headline to the far left and a supporting value to the far right with no connecting line.
*   **Do** embrace extreme whitespace. Let the dark background provide breathing room for the "High-Tech" elements to shine.
*   **Do** use `0.75rem` (Space 3.5) for tight groupings and `1.75rem` (Space 8) for sectional breaks.

### Don't
*   **Don't** use pure black (#000000). Always use the warm darks (#111317) to maintain the premium, "Linear-inspired" depth.
*   **Don't** use standard "drop shadows." If an element needs to stand out, make it lighter in color (Tonal Lift).
*   **Don't** use 100% opaque borders. They clutter the 720x1280 viewport and break the "Monolith" feel.