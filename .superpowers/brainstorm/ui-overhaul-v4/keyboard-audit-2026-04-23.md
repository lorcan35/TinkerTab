# Keyboard + Input UX Audit — 2026-04-23

**Scope:** `main/ui_keyboard.c` (1143 LOC), `main/chat_input_bar.c` (352 LOC), interaction with `main/chat_msg_view.c` scroll.

**Verdict:** the keyboard is **functionally correct** (keys type, Done submits, shift works) but UX-wise it feels unpolished because **two different input surfaces overlap**, common chat punctuation needs a layer switch, and 51 % of the screen is chrome the moment you tap it. Below is a concrete rework plan, ordered by impact-per-hour.

---

## The 15-problem list (what you're actually feeling)

### A — Two input surfaces fight each other

1. **Redundant preview row.** The keyboard panel has its own preview row at [ui_keyboard.c:298-368](../../../main/ui_keyboard.c#L298) — mirrors the last 32 chars of the textarea, + its own mic button, + its own send button. The chat input pill ([chat_input_bar.c:169-276](../../../main/chat_input_bar.c#L169)) already shows typed text, already has the voice ball, and already submits on Done. The keyboard's preview row is dead weight: every control it offers is one-tap away on the pill.
2. **Two send buttons.** Preview row has one ([ui_keyboard.c:347-362](../../../main/ui_keyboard.c#L347)); the Enter key does the same thing ([ui_keyboard.c:968-976](../../../main/ui_keyboard.c#L968)). Users don't know which to tap.
3. **Two mic buttons.** Preview row has one ([ui_keyboard.c:327-344](../../../main/ui_keyboard.c#L327)); pill has the voice ball. Preview-row mic hides keyboard + opens voice overlay — but the ball does the same. Pick one.
4. **Floating trigger circle** at bottom-right ([ui_keyboard.c:896-922](../../../main/ui_keyboard.c#L896)) exists on `lv_layer_top`. The pill already has a dedicated keyboard button ([chat_input_bar.c:245-261](../../../main/chat_input_bar.c#L245)). Two different 60 px targets both open the keyboard.

### B — Chrome eats the screen

5. **Keyboard 500 px + pill 108 px + bottom pad 40 px = 656 px (51 % of 1280 px)** the moment you open the keyboard. That leaves ~624 px for chat — 3-4 bubbles. The preview row alone costs ~80 px of that.
6. **Keyboard height is hardcoded** ([ui_keyboard.c:41](../../../main/ui_keyboard.c#L41)). A compact mode (say, 380 px — no preview, keys only) would claw back real estate for chat.

### C — Typing feels clunky

7. **`!` and `?` require a layer switch.** Letter row 3 has `123 · , · space · . · Done` ([ui_keyboard.c:594-644](../../../main/ui_keyboard.c#L594)). Every exclamation or question needs: tap `123` → tap target → tap `ABC`. Three taps for one character.
8. **"Done" label on Enter is ambiguous.** ([ui_keyboard.c:641](../../../main/ui_keyboard.c#L641)). Does it send, close the keyboard, or both? (It does both: [ui_keyboard.c:968-976](../../../main/ui_keyboard.c#L968).) Should be a paper-plane glyph or just "Send".
9. **Space bar is labeled with a literal single space** ([ui_keyboard.c:629](../../../main/ui_keyboard.c#L629)). Renders as a blank key — hard to visually locate in a dim UI. iOS/Android show "space" at reduced opacity or a wide pill with subtle dots.
10. **Shift has three states (idle / shift / caps) conveyed only by background-color shift** ([ui_keyboard.c:1080-1094](../../../main/ui_keyboard.c#L1080)). Filled vs. outline arrow glyphs are the standard tell; color alone is inaccessible and invisible on glance.
11. **Auto-capitalization is missing.** After `.` + space you still get lowercase. Every sentence start needs a shift tap.
12. **Auto-unshift after one letter** ([ui_keyboard.c:954-958](../../../main/ui_keyboard.c#L954)) makes typing an acronym or name ("WiFi", "OpenAI") a shift-tap per letter. iOS treats double-tap-shift as caps-lock, which this does implement — but no visual distinction from single shift.
13. **No long-press alternates.** Long-press `.` → `,;:/?!` pop-out would collapse problem #7. Long-press `a` → `áàäâ`. Zero of these exist.
14. **No cursor movement.** Users can only append + backspace. Typo three words back = retype everything after.
15. **STT partial-caption label overlaps chat.** ([chat_input_bar.c:263-269](../../../main/chat_input_bar.c#L263)) — placed at `pill_y - 24`, one line tall. A long partial wraps into the chat. Needs either truncation-with-ellipsis, or a dedicated reserved strip.

---

## Scroll bug — already fixed in this session

Separate but related ([REPORT details below](#scroll-fix-chat_msg_viewc-142)). Was: every scroll event triggered a refresh that force-snapped you back to the bottom. Now: we track a `user_scrolled_up` latch and only auto-pin to bottom while the user is already there or while streaming. Code landed in `chat_msg_view.c`.

---

## Recommended rework plan

### Phase 1 — kill the noise (≤ 2 hours, highest impact)

Drop the preview row, the floating trigger, and one of the two send surfaces. Result: keyboard drops from 500 → ~380 px, chrome goes from 656 → ~536 px (42 % of screen, still tall but not oppressive).

```diff
--- ui_keyboard.c
-#define KB_HEIGHT       500
+#define KB_HEIGHT       380
-#define PREVIEW_H       60
-#define PREVIEW_Y       12
+/* preview row removed — chat pill + textarea already show typed text */

 static void build_keyboard_panel(void) {
     ...
-    build_preview_row();
-    int keys_top = PREVIEW_Y + PREVIEW_H + 8;
+    int keys_top = 12;  /* just past the drag handle */
     ...
 }
```

And gate the floating trigger on "screen has no input bar" (home, wifi-setup) — hide it in chat and notes where the pill's kb button already exists:

```c
void ui_keyboard_set_trigger_visible(bool v) {
    if (!s_trigger_btn) return;
    if (v) lv_obj_clear_flag(s_trigger_btn, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(s_trigger_btn, LV_OBJ_FLAG_HIDDEN);
}
// call ui_keyboard_set_trigger_visible(false) from ui_chat.c on show()
```

### Phase 2 — cheap wins on the letter layer (≤ 1 hour)

Replace `Done` with a paper-plane/arrow glyph; move `!` and `?` into the letter layer by swapping them with `,` and `.` (which chat uses less than exclamation/question):

```diff
-/* comma */
-lv_obj_t *comma = make_key(row, ",", comma_w, ...);
+lv_obj_t *excl  = make_key(row, "!", comma_w, ...);
...
-/* period */
-lv_obj_t *dot   = make_key(row, ".", dot_w, ...);
+lv_obj_t *qmark = make_key(row, "?", dot_w, ...);
```

Move `,` and `.` onto the second row's right edge (iOS/Gboard pattern) or behind long-press. And:

```diff
-lv_obj_t *enter = make_key(row, "Done", enter_w, KEY_H,
-                            KB_KEY_ENTER, KB_CYAN, FONT_NAV, KEY_ENTER);
+lv_obj_t *enter = make_key(row, LV_SYMBOL_RIGHT, enter_w, KEY_H,
+                            KB_KEY_ENTER, KB_CYAN, FONT_KEY, KEY_ENTER);
```

Label the spacebar visibly:

```diff
-lv_obj_t *space = make_key(row, " ", space_w, ...);
+lv_obj_t *space = make_key(row, "space", space_w, KEY_H,
+                            KB_SPACE_BG, KB_TEXT_DIM, FONT_NAV, KEY_SPACE);
```

### Phase 3 — auto-behaviors (≤ 3 hours)

- **Auto-capitalize.** Track `s_auto_cap = true` on start + after `.!?` + space. `key_press_cb` consults it before emitting.
- **Shift glyph state.** Two images: outline-arrow (idle), filled-arrow (shift), filled-arrow-with-bar (caps). Swap `LV_SYMBOL_UP` to two custom inline SVGs or just flip letter-case on the shift key itself so users see the change.
- **Long-press alternates.** LVGL supports `LV_EVENT_LONG_PRESSED`. Add a popover with up to 8 alternates for `. , e a o u n` and the number row. Saves most users from ever tapping 123.

### Phase 4 — nice-to-haves (optional)

- Cursor move by long-pressing space (iOS pattern: space turns into a trackpad).
- Emoji layer — a third toggle from `123 → 😀`. 96 common emojis fit in a 4-row grid.
- Recent-word strip above keys (top ~44 px). Requires a tiny in-memory freq map over N messages.
- Swipe-typing — probably out of scope; LVGL doesn't have this built-in.

---

## Other things I'd change while I'm in there

- [chat_input_bar.c:239](../../../main/chat_input_bar.c#L239) — ghost text says `"Hold to speak · or type"`. The bullet wastes horizontal space. `"Speak or type"` reads cleaner and matches iOS "Message" style.
- [chat_input_bar.c:263-269](../../../main/chat_input_bar.c#L263) — STT partial bleeds into chat. Wrap in a pill with `LV_OPA_80` backdrop + single-line truncation, or park it inside the pill itself instead of floating above.
- [ui_keyboard.c:1008-1025](../../../main/ui_keyboard.c#L1008) — `trigger_click_cb` creates a "fallback textarea" when no target is set. This is a code smell and leaks visual state in wifi/notes flows. Delete it; make the caller responsible for wiring a target before showing.

---

## Scroll fix (chat_msg_view.c #142)

Already landed in the build flashed at 00:52 local. The refresh no longer force-jumps to bottom if the user has dragged up more than 48 px. Re-pins when:

- The user explicitly scrolls back to bottom (`chat_msg_view_scroll_to_bottom` clears the latch).
- A new assistant turn begins streaming (`begin_streaming` clears it).

Visual verification is pending — Tab5 went into a W15-C08 WiFi flap loop right after the flash and its debug server is unreachable from my side. The firmware image carries the fix; you should see it working now that uptime has stabilized. If not, a `watchdog-reset` via esptool (or a power cycle) will clear the flap.

---

## Why TC said "let me help you choose"

Reading [/var/log @ 20:42-20:46 UTC](../tc-skill-2026-04-22/voice.log), the exchange went:

```
you   : install clawhub
TC    : ClawHub isn't a CLI — it's a web registry at clawhub.com…
you   : find and install a cool useful skill
TC    : You already have 161 skills installed locally! … I couldn't fetch the
        ClawHub API directly (different domain redirects + no public API).
        … What kind of skill would be useful? Examples: …
```

Three compounding problems, all on the gateway side:

1. **MCP is dead** — every turn the log shows all 6 servers (`sequential-thinking`, `time`, `fetch`, `memory`, `git`, `minimax_coding_plan`) failing to spawn with `MCP error -32000: Connection closed`. So TC has no `fetch` MCP to hit an external API, no `git` MCP to clone, no `sequential-thinking` to plan a multi-step install.
2. **Browser tool timed out** — [gateway log line "browser failed: timed out"](../tc-skill-2026-04-22/gateway.log) came back at 20:46:03. The agent was explicitly told: *do not retry; use an alternative approach or inform the user.* So TC did what the tool instructed — informed you.
3. **"ClawHub" is a hallucination.** No such public registry exists as described. TC's training-era knowledge mixed up the OpenClaw skills dir pattern with a non-existent marketplace, and it passed on bad info.

Stripped of agency tools + handed a fake target, TC fell back to the safest move an LLM knows: ask the user to clarify. That's not a regression in TC; that's TC correctly refusing to fabricate an install command when its tools are all failing.

**The fix is gateway-side**: either restore MCP (still broken despite restart — likely PATH or Node version regression after `openclaw-auto-updater` ran) or stub out the dead tools from the agent's system prompt so it doesn't mention them. Until then, TC in mode=3 is limited to whatever `exec` + `fs.writeFile` can accomplish — which, as the `tab_pulse` skill round-trip proved, is plenty for well-scoped asks. Keep prompts concrete ("write X to Y") and TC delivers. Ask open-ended research ("find me something cool") and it has no tools to look with.
