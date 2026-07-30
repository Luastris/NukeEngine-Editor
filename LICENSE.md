# NukeEngine License, Version 1.1

A **source-available, community-driven, non-compete** license by **Luastris**
(https://luastris.com). This is **not** an OSI-approved "open source" license: the source
is open to read, use, and modify, but redistribution of the Engine itself is restricted,
and Section 5 sets out what the Engine may not be used for.

These terms protect the collective work of Luastris **and every Contributor**, not the
Licensor alone: anyone may use and improve the Engine, but no one may republish it — or
another Contributor's work within it — as a competing or proprietary fork. The Engine is
meant to be built together; improvements flow back to the shared project or stay internal,
never into a rival version.

> **Draft.** This text expresses the licensor's intent in plain terms. It has **not** been
> reviewed by a lawyer. Before you rely on it, have counsel review it for enforceability in
> your jurisdiction. Contributions are accepted only under the CLA referenced in Section 9.

Copyright © St. Evil (Luastris). All rights reserved except as expressly granted below.

---

## 1. Definitions

- **Licensor** — **St. Evil**, the Engine's author and copyright holder, who develops and
  publishes the Engine under the studio name **Luastris**.
- **Official Repository** — the canonical source repository published by the Licensor.
- **Engine** — the software provided under this License in the Official Repository: the
  engine library, the Editor, the built-in modules/renderers, the Player source, and any
  other code the Licensor distributes under this License. It does **not** include the Games,
  content, or Plugins that You create.
- **Player Runtime** — the compiled player/runtime that executes a Game.
- **Game** — a project You create (its content, assets, scripts, and Plugins) together with
  the Player Runtime needed to run it. The Game is Your product, not part of the Engine.
- **Plugin** — a module loaded by the Engine or Player Runtime through its plugin API.
- **Mod** — content that modifies, extends, or adds to a Game, in any form (a `.numod`
  overlay, a loose content or script drop-in, a Plugin written for that Game, or a patched
  build), whether or not it uses the plugin API.
- **Third-Party Component** — software the Engine incorporates, links, vendors, or fetches
  that the Licensor did not author, together with its own license (see Section 16).
- **Contribution** — a modification of the Engine that You submit to the Official Repository.
- **You** — any person or entity exercising rights under this License.
- **Fork** — a copy of the Engine, modified or not, that You **publish or distribute** to
  third parties. Keeping a modified copy for Your own (or Your organization's) internal use
  is **not** a Fork.

## 2. Grant

Subject to the conditions and restrictions below, the Licensor grants You a worldwide,
royalty-free, non-exclusive, non-transferable license to:

1. **use, build, and run** the Engine to develop Games, including for commercial purposes,
   at no charge;
2. **modify** the Engine for Your own development; and
3. exercise the distribution rights in Section 3.

The Licensor retains all rights in the Engine and, as the copyright holder, is **not bound by
this License** in the use of its own work: the Licensor's own Games, modifications, and
internal use are unrestricted and require no attribution under Section 4. This License is a
grant to **You**, not a limitation on the author.

## 3. What You may distribute

1. **Your Game.** You may distribute Your Game — including the Player Runtime, which You may
   modify — freely, commercially or not, as the runtime **of that Game**. You may not
   distribute the Player Runtime, modified or not, as a general-purpose engine or SDK for
   third parties to build their own Games (see Section 5).
2. **The Editor for modding.** If You want players to mod Your Game, direct them to the
   official Editor build. You may, though it is discouraged, bundle the **unmodified**
   official Editor with Your Game.
3. **Plugins.** You may create, use, distribute, and **sell** Plugins, including proprietary,
   closed-source ones, on Your own terms (see Section 8) — with one limit: charging for a
   Plugin or Mod made for **someone else's Game** needs that Game developer's permission
   (Section 5.8).
4. **Contributions.** You may submit modifications of the Engine to the Official Repository
   (see Section 4 and Section 9).

No other distribution of the Engine is permitted (see Section 5).

## 4. What You must do (conditions)

1. **Attribution.** Every Game You distribute, and any distribution permitted by Section 3,
   must credit the Engine — "Powered by NukeEngine — Luastris (luastris.com)" — and link to
   the Official Repository, in a reasonable place (credits, about screen, or documentation).
2. **Disclose modifications.** If the Game was built with a modified Engine or a modified
   Editor (Section 6), the attribution must additionally state that it was made with a
   **modified** version of the Engine/Editor and link to the Official Repository (the
   original).

## 5. What You may not do (restrictions)

1. **No redistribution of the Engine.** You may not distribute the Engine — in whole or in
   part, modified or unmodified, in source or compiled form — to any third party, **except**:
   (a) as a Contribution to the Official Repository; (b) the Player Runtime as the runtime of
   Your Game (Section 3.1); or (c) the unmodified official Editor bundled with Your Game
   (Section 3.2). In particular, You may not provide the Engine (or any modified Engine) to
   third parties as a tool, runtime, framework, or SDK for building their own Games.
2. **No competing fork.** You may not publish or distribute a Fork (Section 1), and in
   particular not one that competes with the Engine or is intended or likely to attract or
   divert the Engine's users or community.
3. **No rebranding or passing off.** You may not present the Engine, or any modified Engine or
   Editor You are permitted to use or distribute, as a different or original engine, or under a
   name or branding that obscures its origin as NukeEngine. A modified Engine or Editor remains
   NukeEngine.
4. **No notice removal.** You may not remove or alter this License or the attribution or
   copyright notices in the Engine.
5. **No ripping of others' Games.** The Engine and Editor read packaged Game data to support
   the development and modding of a Game. You may not use the Engine or Editor — and may not
   modify them — to **extract another party's content** (assets, video, audio, music, art,
   scripts, or any other files) **from that party's Game in order to publish, redistribute,
   or otherwise make that content available**, unless the Game's rightsholder has expressly
   permitted it (for example, in the Game's mod policy). A rightsholder who wants their
   Game's resources public can simply publish the files; absent such permission, extraction
   for publication is a breach of this License in addition to whatever rights the content's
   owner holds. Each Game's rightsholder is an **intended third-party beneficiary** of this
   Section and may enforce it directly against the violator. Opening Your **own** Game's
   packages, and modding a Game within the permissions its rightsholder grants, are not
   restricted by this Section.
6. **No unlawful or abusive use.** This Section is read **narrowly**: it addresses
   **real-world harm**, not the subject matter of fiction. What a lawful Game depicts —
   violence, war, crime, horror, mature or adult themes — is the developer's own business and
   responsibility (rating, age-gating, store and legal compliance), and the Licensor does not
   police themes or artistic content. What You may not do is use the Engine, the Editor, the
   Player Runtime, or **any part or modified version** of them — nor any Game, Plugin, or Mod
   made with them — to commit a crime, or to build, operate, or distribute anything whose
   **purpose or predominant use** is unlawful where it is made available, or that does
   real-world harm in any of these ways:
   1. sexual content involving minors, or material that sexualizes minors — whether the
      depiction is of a real child or a drawn, rendered, or synthetic one. This is the one
      item where "it's fiction" is **not** a defence, and it is not negotiable.
   2. material that, in the real world, incites, recruits for, finances, or gives operational
      instruction for terrorism, mass violence, or violent extremism; or that targets a real
      person or a real group of people with threats, hatred, or harassment for who they are.
      The test is real-world incitement or targeting — a Game merely *depicting* a conflict,
      a faction, or a villain, however repellent, is not that.
   3. intimate or sexual imagery of a real person, made or shared without their consent,
      including synthetic likenesses ("deepfakes").
   4. malware, spyware, ransomware, botnets, cryptojacking, or other software built to damage
      systems or to gain unauthorized access to systems or data.
   5. fraud, phishing, or the theft of money, credentials, or personal data.
   6. surveillance, profiling, or targeting of people in violation of applicable law.

   This restriction attaches to **every** copy of the Engine, modified or not, and the
   internal-use path in Section 6 is **not** an exception to it. No agreement under Section 7
   can authorize use prohibited by this Section.
7. **No real-money gambling or paid randomized rewards.** This Section does **not** restrict
   ordinary commerce. Selling Your Game, and selling content for it at a **fixed price where
   the buyer gets exactly what they paid for** — DLC, expansions, cosmetics, skins, items,
   season passes, subscriptions, microtransactions, whether sold inside or outside the Game —
   is Your business and is not limited here in any way. What You may not use the Engine for is
   **selling chance**: (a) gambling for money or for anything exchangeable for money — casino
   games, betting, wagering, "social casino" titles that sell chips — whether or not licensed,
   including any mechanic that pays winnings back out in money or real-world value; or
   (b) loot boxes, gacha, or other randomized-reward mechanics where what the player gets for
   real money (or for a currency or item bought with real money) is decided by chance.
   Randomized rewards a player **earns by playing** — loot drops, crafting, procedural
   generation, roguelike runs, card packs earned in-game — are expressly permitted. The line
   is simple: **paid content is deterministic; random content is earned.** A purchase must
   never be a bet. The Licensor does not expect to waive this term; any exception requires a
   separate written agreement (Section 7).
8. **No paid Plugins or Mods for someone else's Game.** You may not **monetize** a Plugin or
   Mod made for a Game that is not Yours — by selling it, or putting it or any of its
   features behind a paywall, subscription, paid tier, early-access gate, or ad-gate, or by
   otherwise requiring payment for access — unless that Game's developer or rightsholder has
   **expressly permitted** paid add-ons (for example in the Game's mod policy or monetization
   terms). This applies with particular force to Games distributed **free of charge**: a free
   Game's audience is not Yours to charge. Distributing such a Plugin or Mod at **no charge**
   is always permitted (subject to the Game's own terms), and a voluntary donation link or
   tip jar that is tied to nothing — not access, not content, not priority — is not
   monetization for the purposes of this Section. A Game's rightsholder is an **intended
   third-party beneficiary** of this Section and may enforce it directly. This Section does
   **not** restrict general-purpose Plugins for the Engine — tools, systems, and content any
   developer can use — which You may sell freely under Section 8.

## 6. Modifications, contributions, and internal use

You may modify the Engine and the Editor freely. For any modification You have exactly two
paths:

- **Contribute it back** to the Official Repository (encouraged — the Engine is
  community-driven and lives on shared improvement); or
- **Keep it internal** — use the modified Engine/Editor for Your own or Your organization's
  internal development and Games.

You are **never required** to contribute. What You may **not** do is publish or distribute a
Fork (Sections 1 and 5). Any Game released from a modified Engine or Editor must satisfy the
Attribution and modification-disclosure conditions (Section 4). Modifying the Engine changes
nothing about the use restrictions: Sections 5.5–5.7 bind every copy, including a modified one
kept entirely for internal use.

## 7. Commercial / custom Engine license

Anything not permitted above — in particular, distributing a modified Engine to third
parties (for example, providing a modified Editor so players can mod Your Game), or licensing
a proprietary Engine derivative — requires a **separate written agreement** with the Licensor.
Contact the Licensor at https://luastris.com. Such an agreement is expected to prohibit
further redistribution of the modified Engine to third parties, and will require the modified
Engine and Editor to **retain their NukeEngine identity and attribution** and not be rebranded
as a different or original engine (Section 5.3). Section 5.6 (unlawful or abusive use) is
outside what any such agreement can grant.

## 8. Plugins and Marketplace

Plugins are Yours; You may license or sell them on Your own terms. Your ownership of a Plugin
does not prevent anyone from independently creating similar functionality, including as an
open Contribution to the Engine. The Licensor may operate a marketplace for Plugins; sales
made through it are governed by that marketplace's separate terms, which may include a
commission payable to the Licensor.

**General-purpose vs. Game-specific.** This freedom to sell is about Plugins for the
**Engine** — tools, systems, renderers, importers, content and gameplay frameworks that any
developer can pick up. A Plugin or Mod aimed at **one particular Game that is not Yours** is a
different thing: You may give it away, but You may not charge for it unless that Game's
developer allows paid add-ons (Section 5.8). Your own Games are Yours to monetize however You
like, within Section 5.7.

## 9. Contributions

By submitting a Contribution, You license it to the Licensor and to every recipient of the
Engine under this License, and You grant the Licensor a perpetual, irrevocable, worldwide,
royalty-free right to use, reproduce, modify, distribute, and **relicense** it (including
under other license terms, such as a commercial license under Section 7). The Licensor may
require You to sign a separate Contributor License Agreement (CLA) before a Contribution is
merged; the CLA governs in case of conflict with this Section.

## 10. Fees and donations

No fee or royalty is required to use the Engine or to distribute Games under this License.
Voluntary donations in support of the project are welcome but are not a condition of any
right granted here.

## 11. Trademarks and no affiliation

1. **Our marks.** This License does not grant rights in the "NukeEngine" or "Luastris" names
   or logos, except for the factual attribution required by Section 4.1.
2. **No affiliation.** NukeEngine is an independent project by Luastris. It is **not**
   affiliated with, sponsored by, endorsed by, or otherwise connected to The Foundry
   Visionmongers Ltd. or its **Nuke** compositing software, nor to any other company,
   product, or project whose name contains "Nuke". Those names and marks belong to their
   respective owners; they are named here **solely for identification**, to state the absence
   of any relationship, and no sponsorship, endorsement, or common origin is claimed or
   implied.
3. **Where the name comes from.** "Nuke" names the engine's own nuclear/atomic nomenclature,
   which runs through the whole design: the fundamental entity of a World is an **Atom**,
   Worlds are built out of Atoms, and releases are codenamed after the chemical elements in
   atomic-number order (Hydrogen, Helium, Lithium, …). NukeEngine has its own logo, its own
   concept, and its own field of use — a modular toolset for **desktop single-player games and
   their modding** — and makes no claim to any other party's product, branding, or goodwill.
4. **Third-party marks.** Names of third-party technologies referred to in the Engine or its
   documentation (for example Vulkan, Direct3D, DirectX, Windows, Visual Studio, .NET, Lua,
   Jolt Physics, Diligent Engine, Dear ImGui, Boost, Steam) are the trademarks or registered
   trademarks of their respective owners and are used for identification only. See Section 16.

## 12. Termination

This License and the rights granted to You terminate automatically if You breach it. They
reinstate if You cure the breach within 30 days of becoming aware of it (whether by notice
or otherwise). Sections 9, 11, 13, and 14 survive termination. Games and Plugins You
distributed in compliance before termination remain licensed to their recipients.

## 13. Disclaimer of warranty

THE ENGINE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
NON-INFRINGEMENT.

## 14. Limitation of liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT SHALL THE LICENSOR BE LIABLE FOR ANY
CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT, OR OTHERWISE, ARISING FROM,
OUT OF, OR IN CONNECTION WITH THE ENGINE OR THE USE OR OTHER DEALINGS IN THE ENGINE.

## 15. Governing law

This License is governed by the laws of the country in which the Licensor resides, without
regard to conflict-of-law rules, and any dispute is subject to the courts having jurisdiction
at the Licensor's place of residence. Copyright in the Engine subsists automatically under the
Berne Convention regardless of registration; nothing in this License depends on the Licensor
being a registered legal entity.

## 16. Third-party components

The Engine stands on other people's work. It incorporates, links, vendors, or fetches
Third-Party Components — among them Diligent Engine (with its own bundled glslang, SPIRV-Tools,
SPIRV-Cross, SPIRV-Headers, Vulkan-Headers, volk, xxHash, DirectXShaderCompiler), Dear ImGui
and ImGuizmo, Jolt Physics, Lua and LuaBridge3, miniaudio, Assimp, Boost, GLFW, glm,
nlohmann::json, stb, zstd/zlib, bgfx (legacy renderer), and the .NET runtime host used by the
C# module.

1. **Each Component keeps its own license.** Every Third-Party Component is licensed to You by
   **its own** rightsholder under **its own** license, which travels with it in the Engine's
   tree (its `deps/`, vendored directory, or the `vcpkg` manifest that fetches it) and governs
   that Component. Nothing in this License grants, restricts, or modifies any right in a
   Third-Party Component, and where this License and a Component's license differ, **the
   Component's own license governs that Component**.
2. **No claim of ownership.** The Licensor claims **no** rights in any Third-Party Component
   and asserts no ownership of it. Copyright and trademarks in each Component belong to its
   authors and owners (Section 11.4). The Licensor's terms in this License cover the Engine as
   a whole and the Licensor's own code within it; they do not purport to add conditions to a
   Component that You obtain separately from its own upstream source.
3. **Your obligations travel with them.** You are responsible for complying with each
   Component's license — including its attribution and notice requirements — in anything You
   build or distribute, and for checking whether a Component is even present in Your build (the
   Engine is modular; a module You do not use ships nothing). The third-party license and notice
   files shipped in the Engine's tree must not be removed or altered (Section 5.4).
4. **Third-party terms are theirs, not ours.** A Third-Party Component's license is between You
   and that Component's rightsholder; the Licensor makes no representation or warranty about it
   (Sections 13 and 14).

---

### Plain-language summary (not part of the License)

- **Make games with it, free, including commercial** — ship your game (and its player)
  however you like. Credit the engine; if you built it with a *modified* engine/editor, say
  so and link the original.
- **Improved something? Two choices:** contribute it back (encouraged — the engine is
  community-driven) **or** keep it for your own internal use. You're never forced to
  contribute.
- **Never publish the engine as a fork** — not source, not compiled, not modified, not to
  compete or pull away the community. Only your **game build** goes out (and, if you want
  mods, the official editor).
- **Want to ship a modified engine to others?** That's a fork — not allowed. Talk to Luastris
  for a separate license (e.g. a modified editor so your players can mod your game).
- **Never rebrand it.** Whatever you're allowed to use or ship — modified or not — stays
  NukeEngine. Don't pass it off as a different or your own engine.
- **Never rip other people's games with it.** Using (or modifying) the engine/editor to pull
  assets, video, or audio out of someone else's game and publish them is a license breach —
  and the game's developer can come after you for it directly. If a developer wants their
  resources public, they'll allow it (or just publish the files themselves).
- **Want to sell a closed feature?** Make it a **plugin** and sell it (a marketplace is
  coming; commission may apply).
- **But don't charge for mods/plugins made for someone else's game** unless that developer
  said you may — especially when their game is **free**. Give it away all you like; a tip jar
  that gates nothing is fine. General-purpose plugins for the *engine* stay freely sellable.
- **Your game's content is your business** — the license doesn't police themes; rating and
  legal compliance are on you. What's banned is **real-world harm**, and that binds modified
  copies and private internal builds too: CSAM (no "it's drawn" loophole), real incitement or
  recruitment for terrorism/mass violence, threats and harassment aimed at real people,
  deepfake sexual imagery of real people, malware, fraud, unlawful surveillance. No paid
  license buys an exception.
- **Sell your game however you like — just never sell chance.** Fixed-price DLC, skins,
  cosmetics, subscriptions, microtransactions where the buyer gets exactly what they paid
  for: all fine, in-game or out. Banned: real-money gambling and paid loot boxes/gacha —
  anything where money buys a roll of the dice or pays winnings back out. Randomness you
  *earn by playing* is expressly fine. Paid = deterministic; random = earned.
- **Third-party code stays under its own license.** Diligent, ImGui, Jolt, Lua, miniaudio,
  Assimp, Boost, GLFW, and the rest belong to their authors — we claim nothing in them, their
  licenses govern them, and you must keep their notices in what you ship.
- **Not the other Nuke.** NukeEngine has nothing to do with Foundry's Nuke compositor or any
  other product with a similar name — own logo, own concept (Atoms, element-named releases),
  own niche: desktop single-player games and modding. Their marks are theirs.
- **Free to use; donations welcome, never required.** The goal is a community-driven engine,
  not rent — and the license protects every contributor's work, not just Luastris's.
