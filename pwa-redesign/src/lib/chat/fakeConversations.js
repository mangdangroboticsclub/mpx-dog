/**
 * Fake conversation data for UI prototyping.
 * Each conversation matches the schema used by conversationStore.js
 * so the existing Chat.svelte logic can work with them.
 */

import { generateUUID } from "../conversationStore.js";

/* ── Helpers ─────────────────────────────────────────────────── */

/** Return a timestamp offset by `minutes` from now */
function ago(minutes) {
  return Date.now() - minutes * 60_000;
}

/** Build a simple conversation object */
function convo(title, messages, updatedAt) {
  return {
    id: generateUUID(),
    title,
    createdAt: updatedAt,
    updatedAt,
    messages,
  };
}

function userMsg(text, ts) {
  return { role: "user", text, ts };
}

function botMsg(text, ts, commands) {
  return { role: "bot", text, ts, commands: commands ?? [] };
}

/* ── Fake Conversations ──────────────────────────────────────── */

const today = Date.now();

export const fakeConversations = [
  // ── 1. Kimchi soup grocery ────────────────
  convo(
    "Help me find the best grocery place…",
    [
      userMsg("Help me find the best grocery place to order all ingredients for making kimchi soup", ago(30)),
      botMsg(
        `**Key Ingredients to Look For**

To make a delicious kimchi soup (Kimchi Jjigae), you'll need:

**Kimchi** 🥬
- Aged, fermented kimchi (the older, the better)
- Look for a bag with visible chili flakes

**Protein** 🥩
- Pork belly (sliced thin) or canned tuna
- Soft tofu (soon tofu or silken)

**Aromatics** 🧅
- Onion, garlic, green onion
- Gochugaru (Korean chili flakes)

**Sauces & Seasonings** 🫙
- Gochujang (chili paste)
- Soy sauce, sesame oil
- Dashida (beef stock powder)

> Would you like me to help find a specific store location near you?`,
        ago(28)
      ),
    ],
    ago(28)
  ),

  // ── 2. Order weekly groceries ─────────────
  convo(
    "Order weekly groceries",
    [
      userMsg("Order weekly groceries", ago(120)),
      botMsg(
        `Here's your **weekly grocery list** based on your recent purchases:

**Produce** 🥦
- Broccoli, spinach, carrots
- Bananas, apples, avocados

**Dairy** 🥛
- Whole milk, Greek yogurt
- Cheddar cheese block

**Pantry** 📦
- Brown rice, quinoa
- Olive oil, balsamic vinegar
- Canned tomatoes, black beans

**Protein** 🥩
- Chicken breast (2 lbs)
- Salmon fillets (4 pieces)

Shall I place the order at your preferred store?`,
        ago(118)
      ),
    ],
    ago(118)
  ),

  // ── 3. Find best discount offers ──────────
  convo(
    "Find best discount offers",
    [
      userMsg("Find best discount offers this week", ago(300)),
      botMsg(
        `**Top Discounts This Week** 🏷️

| Store | Offer | Discount |
|-------|-------|----------|
| 🛒 FreshMart | Organic vegetables | 20% off |
| 🏪 GreenGrocer | Dairy products | Buy 1 Get 1 |
| 🏬 SuperSaver | All meats | 15% off |
| 🥦 VeggieHut | Fruits | 30% off on app |

> 💡 Tip: Stack your FreshMart rewards card for extra 5% off on Tuesdays!`,
        ago(298)
      ),
    ],
    ago(298)
  ),

  // ── 4. Hotel and travel deal ──────────────
  convo(
    "Help me find the best deal for a hotel and…",
    [
      userMsg("Help me find the best deal for a hotel and flight to Tokyo next month", ago(600)),
      botMsg(
        `**Best Travel Deals — Tokyo Next Month** ✈️

**Flight Options:**
| Airline | Price | Duration |
|---------|-------|----------|
| ✈️ SkyWays | $580 | 12h |
| ✈️ JetGlobal | $620 | 11h 30m |
| ✈️ EcoFly | $499 | 14h (1 stop) |

**Hotel Recommendations:**
| Hotel | Price/night | Rating |
|-------|-------------|--------|
| 🏨 Shibuya Comfort Inn | $89 | ★★★★☆ |
| 🏨 Shinjuku Grand Hotel | $120 | ★★★★★ |
| 🏨 Asakusa Traditional Stay | $65 | ★★★☆☆ |

> Would you like me to book any of these?`,
        ago(598)
      ),
    ],
    ago(598)
  ),

  // ── 5. Robot dance moves ─────────────────
  convo(
    "Teach my robot some dance moves",
    [
      userMsg("Can you help me create a dance routine for my robot?", ago(900)),
      botMsg(
        `**Robot Dance Routine** 🤖💃

Here's a simple 4-step routine:

1. **The Wave** 🌊
   - Lift front legs sequentially
   - Hold each for 0.5s
   - Repeat 3 times

2. **The Spin** 🔄
   - Rotate 90° right
   - Pause
   - Rotate 90° left
   - Repeat twice

3. **The Bow** 🙇
   - Lower front torso
   - Hold 1 second
   - Rise back up
   - Add a beep sound effect!

4. **The Victory** 🏆
   - Lift one front leg
   - Lift opposite back leg
   - Hold balance for 2 seconds

> I can generate the Lua script for any of these moves! Would you like me to?`,
        ago(898)
      ),
    ],
    ago(898)
  ),

  // ── 6. Weather check ─────────────────────
  convo(
    "What's the weather today?",
    [
      userMsg("What's the weather like today? Should I walk my robot outside?", ago(1100)),
      botMsg(
        `**Today's Weather** ☀️

| Metric | Value |
|--------|-------|
| 🌡️ Temperature | 72°F (22°C) |
| ☁️ Conditions | Partly cloudy |
| 💧 Humidity | 45% |
| 💨 Wind | 8 mph |
| 🌧️ Rain chance | 10% |

> ✅ Perfect weather for a robot walk! Just avoid puddles.`,
        ago(1098)
      ),
    ],
    ago(1098)
  ),

  // ── 7. Recipe suggestion ─────────────────
  convo(
    "Suggest a quick dinner recipe",
    [
      userMsg("I need a quick dinner recipe under 20 minutes", ago(1400)),
      botMsg(
        `**Quick 15-Min Garlic Butter Pasta** 🍝

**Ingredients:**
- 200g spaghetti
- 3 tbsp butter
- 4 cloves garlic (minced)
- Parmesan cheese
- Fresh parsley
- Salt & pepper

**Steps:**
1. Boil pasta according to package
2. Meanwhile, melt butter in a pan
3. Sauté garlic for 1 min until fragrant
4. Toss in cooked pasta
5. Top with Parmesan and parsley

> ⏱️ Total time: ~15 minutes!`,
        ago(1398)
      ),
    ],
    ago(1398)
  ),
];

/**
 * Save fake conversations into localStorage for demo purposes.
 * Only writes if no conversations exist yet (first-time setup).
 */
export function seedFakeConversations() {
  try {
    const raw = localStorage.getItem("mpx_chat_conversations");
    if (raw && Object.keys(JSON.parse(raw)).length > 0) return; // already have data

    const map = {};
    for (const c of fakeConversations) {
      map[c.id] = c;
    }
    localStorage.setItem("mpx_chat_conversations", JSON.stringify(map));
  } catch {
    // ignore localStorage errors
  }
}
