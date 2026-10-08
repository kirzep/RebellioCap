# Screenshot provenance

These are screenshots of RebellioCap's current, rendered application components, captured on **8 October 2026**. The UI is in Russian. They were captured in Microsoft Edge through Playwright at **1440 × 1120**, with the production styles, fonts, icons, and application artwork. PNG compression is lossless; the images were not retouched, composited, or generated as UI mockups.

| File | What it shows |
| --- | --- |
| [overview.png](overview.png) | Recording overview with a 120-second replay buffer, continuous-recording controls, and recent demo clips. |
| [clips.png](clips.png) | Clip library with search, folder navigation, clip actions, and six demo entries. |
| [editor.png](editor.png) | Clip editor with a video preview, separate video/audio tracks, an added text track, composition settings, and export controls. |
| [onboarding.png](onboarding.png) | First-run setup at the screen and recording-quality step. |

## Browser fixtures

The screenshots render the real `App`, `HomeScreen`, `ClipsScreen`, `ClipEditor`, and onboarding components served by the repository's Vite development server. They use **browser fixtures for host responses**, following the approach used by the frontend acceptance tests. The replay-ready state, demo device catalog, clip metadata, and editor import/window callbacks are fixture values. The onboarding capture uses the existing `apps/desktop/e2e/fixtures/first-run.tsx` host.

The six library thumbnails use gameplay screenshots supplied by the repository owner for this documentation. They show Deadlock, Minecraft, Satisfactory, Dota 2, Counter-Strike 2, and an adventure-game scene. The editor preview uses the supplied Satisfactory still, encoded into a six-second demonstration MP4 with a test audio track. It is a still-image presentation, not a recorded gameplay sequence. Thumbnail resizing preserves each image's aspect ratio.

Clip names, dates, file sizes, and the `C:/Clips` location are demonstration values. The text overlay was added through the editor's actual controls. Gameplay imagery is included to illustrate the interface; game assets and trademarks belong to their respective owners. No project license is granted over them, and no affiliation with the game publishers is implied. Original attachments and capture tooling remain separate from the public documentation images.

These images demonstrate the current interface. They are **not evidence of native Windows capture, hardware encoding, Tauri IPC, system checks, or successful video export**. Those capabilities require separate native validation.

The capture checks confirmed meaningful rendered content, no framework error overlay, and no browser console or page errors. The same run exercised the replay-save success state, library folder grouping, editor text insertion and composition-format selection, and progression from the first setup step to the second. No application or acceptance-test source files were changed for these captures.
