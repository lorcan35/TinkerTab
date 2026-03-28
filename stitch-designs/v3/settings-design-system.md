# Design System Documentation: Technical Elegance & Tonal Depth

## 1. Overview & Creative North Star
**Creative North Star: The Monolithic Interface**
This design system rejects the "web-template" aesthetic in favor of a high-precision, technical instrument. It is inspired by the disciplined monochrome of high-end hardware and the warm, atmospheric depth of professional productivity tools. 

We move beyond standard layouts by using **Intentional Asymmetry** and **Tonal Nesting**. Instead of using lines to box content, we use the 8px grid to create "voids" of negative space that guide the eye. The interface should feel like a single, machined block of material where functions are revealed through light and subtle shifts in surface depth rather than applied decoration.

---

## 2. Colors & Surface Logic
The palette is rooted in a "Warm Charcoal" spectrum, moving from the deep `#0D0F12` base to the vibrant `Electric Cyan` accent. 

### The "No-Line" Rule
**Explicit Instruction:** Prohibit 1px solid borders for sectioning or layout containment. Traditional "boxes" make an interface feel cheap and cluttered. Boundaries must be defined solely through background color shifts.
*   *Correct:* A `surface-container-high` card sitting on a `surface` background.
*   *Incorrect:* A `surface` card with a `#2D3444` border.

### Surface Hierarchy & Nesting
Treat the UI as a series of physical layers. Use the following tiers to define importance:
- **Base:** `surface_container_lowest` (#0C0E11) - Use for the primary background.
- **Level 1:** `surface_container_low` (#1A1C1F) - Use for secondary content areas or sidebars.
- **Level 2:** `surface_container_high` (#282A2D) - Use for primary interactive cards.
- **Level 3:** `surface_container_highest` (#333538) - Use for active states or floating elements.

### The "Glass & Soul" Rule
To prevent the dark aesthetic from feeling "flat," use `surface_tint` (#00daf3) at 5-10% opacity for subtle overlays on hero elements. For floating elements, use `backdrop-blur` (20px+) with a semi-transparent `surface_variant` to allow the underlying technical grid to bleed through.

---

## 3. Typography
The system utilizes a high-contrast typographic scale to create an editorial feel.

*   **Headlines (Space Grotesk):** Our technical soul. Use `display-lg` (3.5rem) with `-0.04em` letter spacing for hero moments. The geometric nature of Space Grotesk should feel "stamped" into the interface.
*   **Body (Inter):** Our functional core. Inter provides maximum legibility at small scales. Use `body-md` (0.875rem) for the majority of text to maintain a spacious, professional feel.
*   **Labels (Inter Mono/Caps):** For technical data points, use `label-sm` (0.6875rem) in all-caps with `+0.1em` tracking to mimic engineering schematics.

---

## 4. Elevation & Depth
In this system, height is an illusion of light, not a shadow.

### The Layering Principle
Depth is achieved by "stacking." A `surface_container_highest` card on a `surface_dim` background creates a natural visual "lift." 

### Ambient Shadows (Floating Only)
When an element must float (e.g., a context menu), use an ultra-diffused shadow:
- **Blur:** 32px - 64px
- **Opacity:** 4-8%
- **Color:** Tinted with `primary` (#c3f5ff) rather than pure black to simulate a glowing technical display.

### The "Ghost Border" Fallback
If accessibility requires a container edge, use a "Ghost Border":
- Token: `outline_variant` (#3b494c)
- Opacity: **15% max**
- *Never use high-contrast solid lines.*

---

## 5. Components

### Buttons
- **Primary:** `primary_container` (#00e5ff) background with `on_primary` (#00363d) text. 12px (`DEFAULT`) radius. 48px height. No border.
- **Secondary:** `surface_container_high` background. No border. Text in `text`.
- **Tertiary:** Transparent background, `primary` text. Use for low-emphasis actions.

### Input Fields
- **Default State:** `surface_container_low` background. No border.
- **Focus State:** `surface_container_high` background with a 1px "Ghost Border" in `primary` at 20% opacity. 
- **Typography:** Placeholder text must use `on_surface_variant` (#bac9cc).

### Cards & Lists
- **Rule:** Forbid divider lines.
- **Implementation:** Use `spacing-6` (1.5rem) of vertical white space to separate list items, or alternate background shades between `surface_container_low` and `surface_container_lowest`.

### The "Status Glyph" (Signature Component)
Instead of large status banners, use a 8px circular "Glyph" in `success`, `warning`, or `error` colors, placed with 16px padding from the headline. This maintains the minimalist, technical aesthetic.

---

## 6. Do's and Don'ts

### Do:
- **Do** embrace "The Void." Leave large areas of the `surface_dim` background empty to emphasize the content that *is* there.
- **Do** use `Electric Cyan` sparingly. It is a high-energy laser; use it only for primary calls to action or critical data points.
- **Do** align everything to the 8px grid. Precision is the key to the "Premium" feel.

### Don't:
- **Don't** use pure black (#000000). It kills the "warmth" of the Linear-inspired aesthetic.
- **Don't** use standard 1px borders to separate the header from the body. Use a subtle shift from `surface_container_low` to `surface`.
- **Don't** use "Drop Shadows" on cards. If it doesn't look elevated through color alone, your tonal hierarchy needs adjustment.