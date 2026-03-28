# Design System Specification: Technical Ambientism

## 1. Overview & Creative North Star
The Creative North Star for this design system is **"The Digital Architect."** 

We are moving away from the "app-on-a-phone" aesthetic toward a specialized, high-fidelity instrument. This system rejects the cluttered, bubble-wrap UI of the last decade in favor of a technical, precise, and ambient environment. It combines the brutalist monochrome discipline of Nothing OS with the sophisticated, warm-dark depth of Linear.

To achieve this, we prioritize **intentional asymmetry** and **tonal depth**. Layouts should feel like a blueprint—organized, purposeful, and breathable. We avoid "standard" grids by using extreme typographic scale shifts and overlapping "glass" layers to create a UI that feels like it’s projected rather than just displayed.

---

## 2. Colors & Surface Logic
This system is built on a foundation of "Warm Darks" (#0D0F12) to prevent the clinical coldness of pure black, while using **Electric Cyan** (#00E5FF) as a high-frequency signal.

### The "No-Line" Rule
**Explicit Instruction:** Designers are prohibited from using 1px solid borders for sectioning. 
Structure is defined solely through background color shifts. A `surface-container-low` section sitting on a `surface` background provides enough contrast to define a boundary without the visual "noise" of a stroke.

### Surface Hierarchy & Nesting
Treat the UI as a series of physical layers. We use Material-style tiers to define importance through luminosity:
- **Surface (Lowest):** #111317 - The base canvas.
- **Surface-Container-Low:** #1a1c1f - Secondary content areas.
- **Surface-Container-High:** #282a2d - Active cards or focused elements.
- **Surface-Bright:** #37393d - Highlighted interactive states.

### The Glass & Gradient Rule
To achieve a "Technical Ambient" feel, floating elements (like modals or high-level overlays) must use **Glassmorphism**.
- **Tokens:** `surface_variant` at 60% opacity with a 20px Backdrop Blur.
- **Signature Texture:** Use subtle, linear gradients for Primary CTAs (transitioning from `primary` #c3f5ff to `primary_container` #00e5ff at a 135° angle). This adds "soul" to the precision.

---

## 3. Typography
Typography is our primary tool for hierarchy. We pair the geometric, technical character of **Space Grotesk** with the neutral, highly legible **Inter**.

- **Display & Headlines (Space Grotesk):** Use for high-impact data points and section titles. The "Technical" vibe is achieved by keeping tracking tight (-2%) and using `headline-lg` (2rem) for maximum contrast against body text.
- **Body & Labels (Inter):** Used for all functional reading and system metadata.
- **The Editorial Shift:** Break the grid by aligning `display-lg` text to the far left while keeping `body-md` content nested in a narrower central column. This creates the "Signature" look of a custom-designed publication.

---

## 4. Elevation & Depth
In this system, light is the architect of space, not shadows.

### The Layering Principle
Depth is achieved by "stacking" luminosity. For example, a card (`surface_container_highest`) placed on a section (`surface_container_low`) creates a natural, soft lift.

### Ambient "Ghost" Shadows
If a floating element requires a shadow to pass accessibility in complex views:
- **Color:** Use a tinted version of `on_surface` (Cyan-tinted dark).
- **Settings:** Blur: 40px, Spread: -5px, Opacity: 6%. 
- It should feel like an ambient glow, not a drop shadow.

### The Ghost Border Fallback
If a boundary is absolutely required for accessibility, use the `outline_variant` token at **15% opacity**. Never use 100% opaque borders; they shatter the "ambient" illusion.

---

## 5. Components

### Buttons
- **Primary:** Gradient fill (`primary` to `primary_container`). White text (`on_primary`). 12px (`md`) radius.
- **Secondary:** Surface-Bright fill, no border.
- **Tertiary:** Ghost style. No background, `primary` text color, visible only on hover with a subtle `surface_container` background shift.

### Input Fields
- **Styling:** Use `surface_container_lowest` for the field background. 
- **States:** On focus, the background shifts to `surface_container_high` and the label moves to `primary` color. 
- **Forbid:** Do not use bottom-line-only inputs or heavy outlines.

### Cards & Lists
- **The Divider Ban:** Explicitly forbid 1px dividers between list items. Use 16px (`4`) of vertical white space or a 2% shift in background color between alternating rows to separate content.
- **Leading Elements:** Icons should be monochromatic (`on_surface_variant`) unless they represent an active status (Success/Error).

### Specialized: The "Glance" Chip
For AI-driven metadata, use a chip with a `surface_variant` background and 40% opacity. Text should be `label-sm`. This provides a technical "HUD" (Heads-Up Display) aesthetic.

---

## 6. Do’s and Don'ts

### Do:
- **Embrace Negative Space:** If a screen feels "empty," you are doing it right. Let the typography breathe.
- **Use Sub-pixel Positioning:** Ensure all elements align to the 8px grid to maintain the "Technical" precision.
- **Layer with Intent:** Only elevate elements that require immediate user action.

### Don’t:
- **Don't Use Pure Black:** It kills the "Warm Dark" depth of the system. Stick to the `background` token (#111317).
- **Don't Use Standard Shadows:** They make the interface feel like a generic web app. Use tonal layering instead.
- **Don't Center Everything:** Use asymmetrical layouts (e.g., left-heavy headlines with right-aligned metadata) to create a premium, editorial feel.
- **Don't Use 1px Dividers:** They are the "lazy" way to define space. Use the Spacing Scale to create separation.

---

## 7. Interaction Logic
- **Touch Targets:** Minimum 48px, even if the visual element is smaller (e.g., a 24px icon inside a 48px hit area).
- **Micro-interactions:** Transitions between `surface` tiers should be 200ms with a `cubic-bezier(0.2, 0, 0, 1)` easing for a snappy, high-end feel.