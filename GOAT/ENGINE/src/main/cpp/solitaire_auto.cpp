#include "include/solitaire_auto.h"
#include "include/ghost_sim.h"
#include "include/license_security.h"
#include "include/il2cpp_exports.h"

#include <dlfcn.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <deque>
#include <initializer_list>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Offsets verified against the installed Solitaire IL2CPP metadata (V28.2.90).
constexpr uintptr_t RVA_SOL_UI_GET_INSTANCE  = 0x049BC0B4UL;
constexpr uintptr_t RVA_SOL_GET_STACK_CARDS  = 0x0516B6D8UL;
constexpr uintptr_t RVA_SOL_MOVE_CARD        = 0x0516B7CCUL;
constexpr uintptr_t RVA_SOL_TALON_CLICKED    = 0x0516A190UL;
// Native SolitaireLocalGs::SyncPlayerCardsMapping(). IDA confirms that this
// reads the authoritative id->value arrays from GameStore and stores them at
// LocalGs + 0xD0. Calling only this narrow sync avoids touching gameplay state.
constexpr uintptr_t RVA_SOL_SYNC_PLAYER_CARDS_MAPPING = 0x051711C4UL;
constexpr uintptr_t RVA_SOL_RAY_MOVE_CARD     = 0x049B71FCUL;
constexpr uintptr_t RVA_SOL_UI_TALON_CLICK    = 0x049B2980UL;
constexpr size_t OFF_SOL_UI_LOCAL_GS          = 0x80;
constexpr size_t OFF_SOL_IS_UPDATING_UI       = 0x12;
constexpr size_t OFF_JARRAY_VALUES            = 0x58;
constexpr size_t OFF_LIST_ITEMS               = 0x10;
constexpr size_t OFF_LIST_SIZE                = 0x18;
constexpr size_t OFF_ARRAY_ITEMS              = 0x20;
constexpr size_t OFF_JVALUE_VALUE             = 0x38;
constexpr size_t OFF_JOBJECT_PROPERTIES        = 0x58;
constexpr size_t OFF_COLLECTION_ITEMS          = 0x10;
constexpr size_t OFF_JPROPERTY_CONTENT         = 0x58;
constexpr size_t OFF_JPROPERTY_NAME            = 0x60;
constexpr size_t OFF_JPROPERTYLIST_TOKEN       = 0x10;
constexpr size_t OFF_SOL_PLAYER_CARDS_MAPPING  = 0xD0;
constexpr size_t OFF_SOL_REVEALED_IDS          = 0xD8;
constexpr size_t OFF_SOL_CARDS_PER_TALON_FLIP = 0xF0;
constexpr size_t OFF_SOL_STACKS                = 0x108;
constexpr size_t OFF_SOL_HS_STACKS             = 0x110;
constexpr size_t OFF_HASHSET_SLOTS             = 0x18;
constexpr size_t OFF_HASHSET_LAST_INDEX        = 0x24;
// Jawaker.Games.Core.GameStore static-field layout in V28.2.90. `gs` is the
// raw JObject received by SetGS, before SolitaireLocalGs masks face-down card
// values for the local player's rendered state.
constexpr size_t OFF_GAMESTORE_STATIC_GS        = 0x38;

// VVIP Engine V4 profile. Physical pacing is accelerated behind an exact-state
// handshake, while full-deal IL2CPP information enables a deeper persistent
// horizon and root-scoped branch memory. Slow devices automatically add delay
// only after a native action fails to materialize.
// Turbo profile: the next action is still blocked by the exact expected-state
// handshake, so these are only minimum visual settle windows. A missed Unity
// transition automatically raises g_pacingBackoffMs for this device/round.
constexpr uint64_t SOL_CARD_ACTION_SETTLE_MS      = 4;
constexpr uint64_t SOL_TALON_ACTION_SETTLE_MS     = 2;
constexpr uint64_t SOL_ACTION_RESULT_TIMEOUT_MS   = 700;
constexpr uint64_t SOL_UI_UPDATE_WATCHDOG_MS      = 240;
// Phone ULTRA only: Unity keeps `isUpdatingUI` raised for most of the visual
// Talon/card animation even though LocalGs has already committed the move. The
// exact-state acknowledgement below remains authoritative, so a shorter bounded
// watchdog removes the visible "heavy flip" pause without blindly double-clicking.
// Other profiles (including the PC emulator build) keep the proven 240 ms gate.
constexpr uint64_t SOL_PHONE_TALON_UI_WATCHDOG_MS = 145;
constexpr uint64_t SOL_PHONE_CARD_UI_WATCHDOG_MS  = 180;
constexpr uint64_t SOL_MAX_PACING_BACKOFF_MS      = 180;
constexpr uint64_t SOL_PACING_BACKOFF_STEP_MS     = 35;
constexpr uint64_t SOL_PACING_RECOVERY_STEP_MS    = 30;
constexpr uint64_t SOL_EXHAUSTED_RETRY_MS         = 650;
constexpr uint64_t SOL_MAPPING_SYNC_RETRY_MS       = 240;
constexpr int SOL_NORMAL_SEARCH_FRAMES             = 1;
constexpr int SOL_ENDGAME_SEARCH_FRAMES             = 2;
constexpr int SOL_MAX_RECOVERY_ESCALATION          = 3;
constexpr int SOL_MAPPING_SYNC_MAX_ATTEMPTS        = 12;

using StringNewFn      = void* (*)(const char*);
using ObjectToStringFn = void* (*)(void*, void**);
using ClassNameFn      = const char* (*)(void*);
using DomainGetFn      = void* (*)();
using DomainAssembliesFn = const void** (*)(void*, size_t*);
using AssemblyImageFn  = void* (*)(const void*);
using ImageNameFn      = const char* (*)(void*);
using ClassFromNameFn  = void* (*)(void*, const char*, const char*);
using StaticDataFn     = void* (*)(void*);
using RayMoveFn        = void (*)(void*, void*, int32_t, void*, void*);
using GetUiFn          = void* (*)(void*);
using GetStackCardsFn  = void* (*)(void*, void*, void*);
using MoveCardFn       = bool  (*)(void*, int32_t, void*, void*, void*);
using TalonClickedFn   = bool  (*)(void*, void*);
using TalonUiFn        = void  (*)(void*);
using SyncPlayerCardsMappingFn = bool (*)(void*);

StringNewFn     g_stringNew = nullptr;
ObjectToStringFn g_objectToString = nullptr;
ClassNameFn      g_classGetName = nullptr;
DomainGetFn      g_domainGet = nullptr;
DomainAssembliesFn g_domainAssemblies = nullptr;
AssemblyImageFn  g_assemblyImage = nullptr;
ImageNameFn      g_imageName = nullptr;
ClassFromNameFn  g_classFromName = nullptr;
StaticDataFn     g_staticData = nullptr;
RayMoveFn        g_rayMove = nullptr;
GetUiFn         g_getUi = nullptr;
GetStackCardsFn g_getStackCards = nullptr;
MoveCardFn      g_moveCard = nullptr;
TalonClickedFn  g_talonClicked = nullptr;
TalonUiFn       g_talonUi = nullptr;
SyncPlayerCardsMappingFn g_syncPlayerCardsMapping = nullptr;
void*           g_ui = nullptr;
void*           g_rayController = nullptr;
void*           g_cardsController = nullptr;
void*           g_gameStoreStatics = nullptr;
void*           g_lastLocalGs = nullptr;

bool     g_enabled = false;
uint64_t g_lastMoveMs = 0;
uint64_t g_lastTalonMs = 0;
bool     g_lastActionWasTalon = false;
// Starts in the fast profile. A physical action that fails to materialize
// raises this bounded delay automatically; successful actions decay it back to
// zero. This preserves speed on healthy devices without turning a slow Unity
// animation into dropped moves or a stalled committed plan.
uint64_t g_pacingBackoffMs = 0;
// The loader selects a device-specific search slice after libengine is loaded.
// These knobs affect virtual planning work only; physical card/talon dispatch
// remains protected by the exact-state acknowledgement and adaptive backoff.
// That separation lets PC emulators spend more CPU per frame without making a
// slower phone drop Unity input or execute a stale committed-plan move.
std::atomic<int> g_searchSliceNodes{2200};
std::atomic<int> g_searchSliceMs{7};
std::atomic<int> g_searchArenaNodes{28000};
std::atomic<int> g_performanceProfile{2};

static bool phoneUltraProfile() {
    return g_performanceProfile.load(std::memory_order_relaxed) == 2;
}

static uint64_t effectiveUiWatchdogMs() {
    if (!phoneUltraProfile()) return SOL_UI_UPDATE_WATCHDOG_MS;

    // Back off automatically on a device/round where Unity rejected an action.
    // Two misses restore almost the original 240 ms window; successful exact
    // acknowledgements decay this penalty again, so healthy rounds stay fast.
    const uint64_t fastBase = g_lastActionWasTalon
        ? SOL_PHONE_TALON_UI_WATCHDOG_MS
        : SOL_PHONE_CARD_UI_WATCHDOG_MS;
    return std::min(SOL_UI_UPDATE_WATCHDOG_MS,
                    fastBase + std::min<uint64_t>(g_pacingBackoffMs, 95));
}

static uint64_t pacingRecoveryStepMs() {
    // The phone fast path should not remain slow for several successful moves
    // after one transient animation miss. PC/default behavior is unchanged.
    return phoneUltraProfile() ? 45 : SOL_PACING_RECOVERY_STEP_MS;
}
int      g_lastMoveId = -1;
uint64_t g_lastMoveAttemptMs = 0;
std::string g_lastMoveFrom;
std::string g_lastMoveTo;
struct MoveRecord {
    int cardId;
    std::string from;
    std::string to;
    uint64_t at;
};
std::vector<MoveRecord> g_moveHistory;
struct PositionRecord {
    std::string tableau;
    uint64_t at;
};
std::vector<PositionRecord> g_positionHistory;
std::vector<PositionRecord> g_solverStateHistory;
int      g_lastRecycleScore = -1000000;
bool     g_rescueMode = false;
int      g_rrSource = 0;
uint64_t g_lastDiagMs = 0;
uint64_t g_lastCardDiagMs = 0;
uint64_t g_lastStallDiagMs = 0;
bool     g_loggedStackKeys = false;
bool     g_loggedStackStorage = false;
bool     g_loggedSnapshot = false;
bool     g_loggedPerfectSnapshot = false;
bool     g_loggedMappingSnapshot = false;
bool     g_loggedHiddenStateSnapshot = false;
std::string g_stackPrefix = "p_0_";
std::atomic_flag g_solitaireAccess = ATOMIC_FLAG_INIT;
int      g_actualRecycleCount = 0;
int      g_cardsPerTalonFlip = 3;
// Values learned during this physical round must survive transient empty
// PlayerCardsMapping snapshots and controller animation gaps.  A value is
// keyed by the game's stable card id; -1 means that card is still opaque.
std::array<int, 52> g_roundCardValues = [] {
    std::array<int, 52> values{};
    values.fill(-1);
    return values;
}();
int      g_lastRoundKnownCount = 0;
uint64_t g_lastPerfectInfoDiagMs = 0;
uint64_t g_lastMappingSyncMs = 0;
int      g_mappingSyncAttempts = 0;
bool     g_mappingValidated = false;

static void resetExpertPlanner();

#define SOL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GOAT_SOL", __VA_ARGS__)

struct Card {
    int id = -1;
    int value = -1;
    // The serialized stack hides a face-down value as -1. CardsController
    // still owns the real value, so planning can remember it without making
    // the physical card movable before the game reveals it.
    int knownValue = -1;
    std::string color;
    // Search-only marker.  When a legal virtual move exposes a card whose
    // value is not currently available from IL2CPP, the reveal itself is
    // still real progress.  Keep the card immovable until the next physical
    // snapshot supplies its value, but do not incorrectly treat it as hidden.
    bool virtuallyRevealed = false;
    bool valid() const { return id >= 0; }
    bool faceUp() const { return id >= 0 && value >= 0 && value < 52; }
    bool known() const { return faceUp() || (knownValue >= 0 && knownValue < 52); }
    bool revealedForPlanning() const { return faceUp() || virtuallyRevealed; }
};

static bool tryEnterSolitaire() {
    return !g_solitaireAccess.test_and_set(std::memory_order_acquire);
}

struct SolitaireAccessGuard {
    ~SolitaireAccessGuard() { g_solitaireAccess.clear(std::memory_order_release); }
};

static void resetRoundState(bool resetControllers) {
    g_lastMoveMs = 0;
    g_lastTalonMs = 0;
    g_lastActionWasTalon = false;
    g_pacingBackoffMs = 0;
    g_lastMoveId = -1;
    g_lastMoveAttemptMs = 0;
    g_lastMoveFrom.clear();
    g_lastMoveTo.clear();
    g_moveHistory.clear();
    g_positionHistory.clear();
    g_solverStateHistory.clear();
    g_lastRecycleScore = -1000000;
    g_rescueMode = false;
    g_rrSource = 0;
    g_actualRecycleCount = 0;
    g_cardsPerTalonFlip = 3;
    g_roundCardValues.fill(-1);
    g_lastRoundKnownCount = 0;
    g_lastPerfectInfoDiagMs = 0;
    g_lastMappingSyncMs = 0;
    g_mappingSyncAttempts = 0;
    g_mappingValidated = false;
    g_lastStallDiagMs = 0;
    g_loggedStackKeys = false;
    g_loggedStackStorage = false;
    g_loggedSnapshot = false;
    g_loggedPerfectSnapshot = false;
    g_loggedMappingSnapshot = false;
    g_loggedHiddenStateSnapshot = false;
    g_stackPrefix = "p_0_";
    resetExpertPlanner();
    if (resetControllers) {
        g_rayController = nullptr;
        g_cardsController = nullptr;
    }
}

static void* listItem(void* list, int index) {
    if (!list || index < 0) return nullptr;
    const int size = *(int*)((uint8_t*)list + OFF_LIST_SIZE);
    if (size <= index || size > 128) return nullptr;
    void* arr = *(void**)((uint8_t*)list + OFF_LIST_ITEMS);
    if (!arr) return nullptr;
    return *(void**)((uint8_t*)arr + OFF_ARRAY_ITEMS + sizeof(void*) * index);
}

static int listSize(void* list) {
    if (!list) return 0;
    const int size = *(int*)((uint8_t*)list + OFF_LIST_SIZE);
    return (size >= 0 && size <= 128) ? size : 0;
}

static void* arrayValue(void* arrayToken, int index) {
    if (!arrayToken || index < 0) return nullptr;
    void* values = *(void**)((uint8_t*)arrayToken + OFF_JARRAY_VALUES);
    return listItem(values, index);
}

static int tokenInt(void* token) {
    if (!token) return -1;
    void* boxed = *(void**)((uint8_t*)token + OFF_JVALUE_VALUE);
    if (!boxed) return -1;
    const int64_t v = *(int64_t*)((uint8_t*)boxed + 0x10);
    return (v >= 0 && v <= 1000000) ? (int)v : -1;
}

static std::string tokenString(void* token) {
    if (!token) return {};
    void* value = *(void**)((uint8_t*)token + OFF_JVALUE_VALUE);
    return readIl2CppString(value);
}

// HsStacks is the game's authoritative hidden-state JObject (SolitaireLocalGs
// +0x110 in V28.2.90).  It is intentionally diagnostic-only here: dumping it
// once per round lets us verify whether this offline mode exposes additional
// card metadata without making the solver depend on an unverified JSON shape.
static void logHiddenStateSnapshot(void* localGs) {
    if (!localGs || g_loggedHiddenStateSnapshot || !g_objectToString) return;
    void* hiddenState = *(void**)((uint8_t*)localGs + OFF_SOL_HS_STACKS);
    if (!hiddenState) return;
    void* exception = nullptr;
    void* serialized = g_objectToString(hiddenState, &exception);
    if (exception || !serialized) {
        SOL_LOG("hs-stacks ptr=%p serialization-failed exception=%p",
                hiddenState, exception);
        g_loggedHiddenStateSnapshot = true;
        return;
    }
    const std::string json = readIl2CppString(serialized);
    constexpr size_t kLogChunk = 2800;
    const size_t parts = std::max<size_t>(1, (json.size() + kLogChunk - 1) / kLogChunk);
    for (size_t part = 0; part < parts; ++part) {
        const size_t offset = part * kLogChunk;
        const size_t count = std::min(kLogChunk, json.size() - offset);
        SOL_LOG("hs-stacks part=%d/%d json=%.*s", (int)part + 1, (int)parts,
                (int)count, json.data() + offset);
    }
    g_loggedHiddenStateSnapshot = true;
}

static void* objectField(void* object, const char* wanted) {
    if (!object || !wanted) return nullptr;
    void* properties = *(void**)((uint8_t*)object + OFF_JOBJECT_PROPERTIES);
    void* items = properties ? *(void**)((uint8_t*)properties + OFF_COLLECTION_ITEMS) : nullptr;
    const int count = listSize(items);
    for (int i = 0; i < count; ++i) {
        void* property = listItem(items, i);
        if (!property) continue;
        const std::string name = readIl2CppString(*(void**)((uint8_t*)property + OFF_JPROPERTY_NAME));
        if (name != wanted) continue;
        void* content = *(void**)((uint8_t*)property + OFF_JPROPERTY_CONTENT);
        return content ? *(void**)((uint8_t*)content + OFF_JPROPERTYLIST_TOKEN) : nullptr;
    }
    return nullptr;
}

static void* objectFieldAny(void* object, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (void* value = objectField(object, name)) return value;
    }
    return nullptr;
}

static Card parseCard(void* token) {
    Card card;
    // The current Solitaire state stores each card as a JObject.  Keep the
    // JArray route for backwards compatibility with older local-state builds.
    void* id = objectFieldAny(token, {"id", "Id", "ID", "cardId", "CardId"});
    void* value = objectFieldAny(token, {"val", "value", "Value", "cardValue", "CardValue"});
    void* color = objectFieldAny(token, {"color", "Color", "suit", "Suit"});
    card.id = tokenInt(id);
    card.value = tokenInt(value);
    if (card.value >= 0 && card.value < 52) card.knownValue = card.value;
    card.color = tokenString(color);
    if (card.id >= 0 || card.value >= 0) return card;

    card.id = tokenInt(arrayValue(token, 0));
    card.value = tokenInt(arrayValue(token, 1));
    if (card.value >= 0 && card.value < 52) card.knownValue = card.value;

    // CardController.ToJArray serializes the color as a nested JArray.  Keep
    // the direct form as a fallback so the reader remains tolerant of a minor
    // game-side serialization change.
    void* colorToken = arrayValue(token, 2);
    card.color = tokenString(colorToken);
    if (card.color.empty()) card.color = tokenString(arrayValue(colorToken, 0));
    return card;
}

static bool isRevealed(void* localGs, int cardId) {
    if (!localGs || cardId < 0) return false;
    void* set = *(void**)((uint8_t*)localGs + OFF_SOL_REVEALED_IDS);
    if (!set) return false;
    const int lastIndex = *(int*)((uint8_t*)set + OFF_HASHSET_LAST_INDEX);
    if (lastIndex < 0 || lastIndex > 80) return false;
    void* slots = *(void**)((uint8_t*)set + OFF_HASHSET_SLOTS);
    if (!slots) return false;

    // HashSet<int>.Slot is { hashCode, next, value }.  Reading the real
    // RevealedCardIds set lets us move an entire exposed sequence rather than
    // only the last card of a tableau column.
    const uint8_t* values = (const uint8_t*)slots + OFF_ARRAY_ITEMS;
    for (int i = 0; i < lastIndex; ++i) {
        const int* slot = (const int*)(values + i * 12);
        if (slot[0] >= 0 && slot[2] == cardId) return true;
    }
    return false;
}

static Card firstMovableTableauCard(void* localGs, const std::vector<Card>& cards) {
    (void)localGs;
    // In this local state, a closed card is encoded as val=-1.  The first
    // non-negative value is precisely the start of the exposed sequence.
    for (const Card& card : cards) {
        if (card.faceUp()) return card;
    }
    return {};
}

// Verified against SolitaireRaycaster.Rank/Suit/CanDropToTableauStack and
// CanDropToFoundationStack in the shipped IL2CPP binary.
static int cardRank(const Card& card) {
    if (!card.faceUp()) return -1;
    return (card.value % 13 + 1) % 13; // Ace=0 ... King=12
}

static int cardSuit(const Card& card) {
    return card.faceUp() ? (card.value / 13) : -1;
}

static int knownCardRank(const Card& card) {
    const int value = card.faceUp() ? card.value : card.knownValue;
    return (value >= 0 && value < 52) ? (value % 13 + 1) % 13 : -1;
}

static int knownCardSuit(const Card& card) {
    const int value = card.faceUp() ? card.value : card.knownValue;
    return (value >= 0 && value < 52) ? value / 13 : -1;
}

static bool canDropOnTableau(const Card& moving, const Card* destination) {
    const int movingRank = cardRank(moving);
    if (movingRank < 0) return false;
    if (!destination) return movingRank == 12; // only a King opens a column
    const int destRank = cardRank(*destination);
    if (destRank < 0) return false;
    return destRank - movingRank == 1 && ((cardSuit(*destination) & 1) != (cardSuit(moving) & 1));
}

static bool canDropOnFoundation(const Card& moving, const Card* destination) {
    const int movingRank = cardRank(moving);
    if (movingRank < 0) return false;
    if (!destination) return movingRank == 0; // Ace starts a foundation
    const int destRank = cardRank(*destination);
    return destRank >= 0 && cardSuit(moving) == cardSuit(*destination) && movingRank == destRank + 1;
}

static bool foundationAdvanceIsSafe(const Card& moving,
                                    const std::array<std::vector<Card>, 4>& foundations) {
    const int rank = cardRank(moving); // Ace=0, Two=1, ... King=12
    const int suit = cardSuit(moving);
    if (rank < 0 || suit < 0) return false;
    if (rank <= 1) return true; // Aces and twos are always free progress.

    // Foundation slots are dynamic, so derive progress by suit.  A missing
    // opposite-colour suit is rank -1, not "unknown": treating it as absent
    // from the minimum was allowing one advanced black/red pile to make an
    // unsafe push look harmless while the other suit was still at zero.
    std::array<int, 4> topBySuit{-1, -1, -1, -1};
    for (const auto& pile : foundations) {
        if (pile.empty()) continue;
        const int topSuit = cardSuit(pile.back());
        const int topRank = cardRank(pile.back());
        if (topSuit >= 0 && topSuit < 4 && topRank >= 0)
            topBySuit[topSuit] = std::max(topBySuit[topSuit], topRank);
    }
    int oppositeMin = 12;
    for (int otherSuit = 0; otherSuit < 4; ++otherSuit) {
        if ((otherSuit & 1) != (suit & 1))
            oppositeMin = std::min(oppositeMin, topBySuit[otherSuit]);
    }
    // A rank >= 3 is safe only if *both* opposite-colour suits are no more
    // than two ranks behind, preserving the cards required for Tableau chains.
    return rank <= oppositeMin + 2;
}

// Measures how many ranks the four foundations have advanced beyond the
// conservative opposite-colour threshold.  Zero means every exposed
// foundation card is still available at a tactically safe height.  Keeping
// this as a debt (instead of a binary flag) lets deep search temporarily push
// one risky blocker only when the same calculated line repays that risk by
// uncovering a card or advancing the lagging foundations.
static int foundationRiskDebt(const std::array<std::vector<Card>, 4>& foundations) {
    std::array<int, 4> topBySuit{-1, -1, -1, -1};
    for (const auto& pile : foundations) {
        if (pile.empty()) continue;
        const int suit = cardSuit(pile.back());
        const int rank = cardRank(pile.back());
        if (suit >= 0 && suit < 4 && rank >= 0)
            topBySuit[suit] = std::max(topBySuit[suit], rank);
    }

    int debt = 0;
    for (int suit = 0; suit < 4; ++suit) {
        const int rank = topBySuit[suit];
        if (rank <= 1) continue; // Aces and twos never create tactical debt.
        int oppositeMin = 12;
        for (int other = 0; other < 4; ++other) {
            if ((other & 1) != (suit & 1))
                oppositeMin = std::min(oppositeMin, topBySuit[other]);
        }
        debt += std::max(0, rank - (oppositeMin + 2));
    }
    return debt;
}

static int firstFaceUpIndex(const std::vector<Card>& cards) {
    for (int i = 0; i < (int)cards.size(); ++i) {
        if (cards[i].faceUp()) return i;
    }
    return -1;
}

static int movableCardIndex(const std::vector<Card>& cards, int movingId) {
    const int first = firstFaceUpIndex(cards);
    for (int i = first; i >= 0 && i < (int)cards.size(); ++i) {
        if (cards[i].id == movingId) return i;
    }
    return -1;
}

static std::string tableauPositionKey(const std::array<std::vector<Card>, 7>& tableau) {
    std::string key;
    key.reserve(220);
    for (const auto& column : tableau) {
        for (const Card& card : column) {
            key += std::to_string(card.id);
            key.push_back(',');
        }
        key.push_back('|');
    }
    return key;
}

static void rememberTableauPosition(const std::array<std::vector<Card>, 7>& tableau, uint64_t now) {
    const std::string key = tableauPositionKey(tableau);
    if (!g_positionHistory.empty() && g_positionHistory.back().tableau == key) return;
    g_positionHistory.push_back({key, now});
    while (g_positionHistory.size() > 40 ||
           (!g_positionHistory.empty() && now - g_positionHistory.front().at > 90000)) {
        g_positionHistory.erase(g_positionHistory.begin());
    }
}

static bool wouldRepeatRecentTableau(const std::array<std::vector<Card>, 7>& tableau,
                                     int from, int to, int movingId, uint64_t now) {
    if (from < 0 || from >= 7 || to < 0 || to >= 7 || from == to) return true;
    const int start = movableCardIndex(tableau[from], movingId);
    if (start < 0) return true;
    auto simulated = tableau;
    std::vector<Card> sequence(simulated[from].begin() + start, simulated[from].end());
    simulated[from].erase(simulated[from].begin() + start, simulated[from].end());
    simulated[to].insert(simulated[to].end(), sequence.begin(), sequence.end());
    const std::string key = tableauPositionKey(simulated);
    for (auto it = g_positionHistory.rbegin(); it != g_positionHistory.rend(); ++it) {
        if (now - it->at > 120000) break;
        if (it->tableau == key) return true;
    }
    return false;
}

static int countProductiveOptions(const std::array<std::vector<Card>, 7>& tableau,
                                  const std::array<std::vector<Card>, 4>& foundations) {
    int options = 0;
    // Count moves that expose a closed card.
    for (int src = 0; src < 7; ++src) {
        const int start = firstFaceUpIndex(tableau[src]);
        if (start < 0 || start == 0) continue;
        const Card& moving = tableau[src][start];
        for (int dst = 0; dst < 7; ++dst) {
            if (src == dst) continue;
            const Card* top = tableau[dst].empty() ? nullptr : &tableau[dst].back();
            if (canDropOnTableau(moving, top)) ++options;
        }
    }
    // Count safe advances to foundation, which also reduce congestion.
    for (int src = 0; src < 7; ++src) {
        if (tableau[src].empty() || !tableau[src].back().faceUp()) continue;
        const Card& moving = tableau[src].back();
        if (!foundationAdvanceIsSafe(moving, foundations)) continue;
        for (int f = 0; f < 4; ++f) {
            const Card* top = foundations[f].empty() ? nullptr : &foundations[f].back();
            if (canDropOnFoundation(moving, top)) { ++options; break; }
        }
    }
    return options;
}

static int structuralScore(const std::array<std::vector<Card>, 7>& tableau,
                           const std::array<std::vector<Card>, 4>& foundations) {
    int hidden = 0;
    int orderedLinks = 0;
    int foundationCards = 0;
    bool availableKing = false;
    for (const auto& column : tableau) {
        for (int i = 0; i < (int)column.size(); ++i) {
            if (!column[i].faceUp()) { ++hidden; continue; }
            if (cardRank(column[i]) == 12) availableKing = true;
            if (i > 0 && column[i - 1].faceUp() &&
                canDropOnTableau(column[i], &column[i - 1])) ++orderedLinks;
        }
    }
    int usefulEmptyColumns = 0;
    for (const auto& column : tableau) {
        if (column.empty() && availableKing) ++usefulEmptyColumns;
    }
    for (const auto& pile : foundations) foundationCards += (int)pile.size();
    return foundationCards * 24 + orderedLinks * 14 + usefulEmptyColumns * 18 - hidden * 120;
}

static bool improvesNextPosition(const std::array<std::vector<Card>, 7>& tableau,
                                 const std::array<std::vector<Card>, 4>& foundations,
                                 int from, int to, int movingId) {
    if (from < 0 || from >= 7 || to < 0 || to >= 7 || from == to) return false;
    const int start = movableCardIndex(tableau[from], movingId);
    if (start < 0) return false;
    const int before = countProductiveOptions(tableau, foundations);
    auto simulated = tableau;
    std::vector<Card> sequence(simulated[from].begin() + start, simulated[from].end());
    simulated[from].erase(simulated[from].begin() + start, simulated[from].end());
    simulated[to].insert(simulated[to].end(), sequence.begin(), sequence.end());
    const int after = countProductiveOptions(simulated, foundations);
    // Equal option counts are useful only when this exact move exposes the
    // face-down card below the moved sequence. This preserves productive
    // preparation while rejecting cosmetic tableau shuffles.
    const bool revealedSomething = start > 0 && !tableau[from][start - 1].faceUp();
    return after > before || (after == before && revealedSomething);
}

static bool hasImmediatelyMovableKing(
        const std::array<std::vector<Card>, 7>& tableau, int excludedSource) {
    for (int src = 0; src < 7; ++src) {
        if (src == excludedSource) continue;
        const int first = firstFaceUpIndex(tableau[src]);
        if (first < 0) continue;
        for (int start = first; start < (int)tableau[src].size(); ++start) {
            // A King at index zero already owns the base of a column. Moving it
            // to a newly emptied column is structural churn, not new mobility.
            if (start > 0 && cardRank(tableau[src][start]) == 12) return true;
        }
    }
    return false;
}

static bool isStrategicTableauMove(const std::array<std::vector<Card>, 7>& tableau,
                                   int from, int start, int to, const Card& moving) {
    if (from < 0 || from >= 7 || to < 0 || to >= 7 || from == to) return false;
    if (start < 0 || start >= (int)tableau[from].size() || !moving.faceUp()) return false;
    const Card* top = tableau[to].empty() ? nullptr : &tableau[to].back();
    if (!canDropOnTableau(moving, top)) return false;
    const bool destinationEmpty = tableau[to].empty();
    const bool sourceWillBeEmpty = (start == 0);
    if (destinationEmpty && cardRank(moving) != 12) return false;
    // A King starting at index zero is already the base of its source stack.
    // Moving it to an empty column changes no tactical relationship and is
    // the classic King-to-empty-column loop. Only a sub-sequence (start > 0)
    // may use an empty column.
    if (destinationEmpty && cardRank(moving) == 12 && start == 0) return false;
    // Whether a non-King clear has a useful future King depends on Waste/Talon
    // state, which is unavailable in this low-level legality helper. The full
    // GameState generator applies that information-aware restriction below.
    (void)sourceWillBeEmpty;
    return true;
}

// A compact, entirely native snapshot used only for planning.  It deliberately
// contains no IL2CPP pointers: searching this state cannot mutate Unity or race
// the animation system.
struct GameState {
    std::array<std::vector<Card>, 7> tableau;
    std::array<std::vector<Card>, 4> foundations;
    std::vector<Card> talon;
    std::vector<Card> waste;
    int recycles = 0;
    int drawCount = 3;
};

enum class VirtualMoveKind : uint8_t { Tableau, Foundation, FoundationToTableau, Talon };
struct Move {
    VirtualMoveKind kind = VirtualMoveKind::Tableau;
    int from = -1;       // tableau source; -1 means waste
    int to = -1;         // tableau/foundation destination
    int start = -1;      // first card of a tableau sequence
};

static int virtualFoundationCount(const GameState& state) {
    int count = 0;
    for (const auto& pile : state.foundations) count += (int)pile.size();
    return count;
}

static bool hasOpaqueCards(const GameState& state) {
    for (const auto& column : state.tableau) {
        for (const Card& card : column) if (!card.faceUp() && !card.known()) return true;
    }
    return false;
}

static int knownCardCount(const GameState& state) {
    int known = 0;
    const auto countPile = [&known](const std::vector<Card>& pile) {
        for (const Card& card : pile) if (card.known()) ++known;
    };
    for (const auto& pile : state.tableau) countPile(pile);
    for (const auto& pile : state.foundations) countPile(pile);
    countPile(state.talon);
    countPile(state.waste);
    return known;
}

static bool hasReachableKingForNewColumn(const GameState& state,
                                         int excludedSource) {
    if (hasImmediatelyMovableKing(state.tableau, excludedSource)) return true;
    if (!state.waste.empty() && knownCardRank(state.waste.back()) == 12) return true;

    // Only count Kings exposed by the next two Draw-N actions. A King buried
    // deeper is not an immediate justification for sacrificing a column; the
    // planner can first advance the Talon and reconsider from the real state.
    const int draw = std::clamp(state.drawCount, 1, 3);
    for (int offset = 0, batch = 0;
         offset < (int)state.talon.size() && batch < 2;
         offset += draw, ++batch) {
        const int exposed = std::min(offset + draw - 1,
                                     (int)state.talon.size() - 1);
        if (knownCardRank(state.talon[exposed]) == 12) return true;
    }
    return false;
}

static void revealVirtualTop(std::vector<Card>& pile) {
    if (pile.empty() || pile.back().faceUp() || pile.back().virtuallyRevealed) return;
    if (pile.back().known()) {
        pile.back().value = pile.back().knownValue;
    } else {
        // The physical move will reveal the real value on the following tick.
        // Previously this returned without changing the virtual state, so a
        // perfectly legal line that exposed an unresolved card could never
        // satisfy hiddenGain and was pruned as a dead end.
        pile.back().virtuallyRevealed = true;
    }
}

static std::string virtualPositionKey(const GameState& state) {
    std::string key;
    key.reserve(520);
    const auto append = [&key](const std::vector<Card>& pile) {
        for (const Card& card : pile) {
            key += std::to_string(card.id);
            key.push_back(card.faceUp() ? 'u' :
                          (card.virtuallyRevealed ? 'r' : 'd'));
            // A position with an unresolved face-down card is not equivalent
            // to the same physical layout after PlayerCardsMapping has
            // supplied its value.  Omitting this field allowed an opaque
            // transposition/dead-state result to poison a later perfect-info
            // search for the same card IDs.
            key.push_back(':');
            key += std::to_string(card.faceUp() ? card.value : card.knownValue);
            key.push_back(',');
        }
        key.push_back('|');
    };
    for (const auto& pile : state.tableau) append(pile);
    for (const auto& pile : state.foundations) append(pile);
    append(state.talon);
    append(state.waste);
    // Recycle count is a score penalty, not a distinct card position.  Leaving
    // it in the key made an identical Draw-N loop look new forever.  Exact
    // talon/waste order already describes reachability completely.
    key += "d" + std::to_string(state.drawCount);
    return key;
}

static std::string solverPositionKey(const GameState& state) {
    // Tableau columns are rule-symmetric. Canonicalising their encodings makes
    // permutations of the same board share one transposition entry, yielding a
    // much deeper search for the same Unity-thread time budget. Physical move
    // validation continues to use virtualPositionKey with real column indices.
    std::array<std::string, 7> columns;
    for (int i = 0; i < 7; ++i) {
        std::string& encoded = columns[i];
        for (const Card& card : state.tableau[i]) {
            encoded += std::to_string(card.id);
            encoded.push_back(card.faceUp() ? 'u' :
                              (card.virtuallyRevealed ? 'r' : 'd'));
            encoded.push_back(':');
            encoded += std::to_string(card.faceUp() ? card.value : card.knownValue);
            encoded.push_back(',');
        }
    }
    std::sort(columns.begin(), columns.end());

    std::string key;
    key.reserve(320);
    for (const std::string& column : columns) {
        key += column;
        key.push_back('|');
    }
    std::array<int, 4> foundationBySuit{-1, -1, -1, -1};
    for (const auto& pile : state.foundations) {
        if (pile.empty()) continue;
        const int suit = knownCardSuit(pile.back());
        if (suit >= 0 && suit < 4) foundationBySuit[suit] = knownCardRank(pile.back());
    }
    key.push_back('f');
    for (int rank : foundationBySuit) {
        key += std::to_string(rank);
        key.push_back(',');
    }
    const auto appendOrdered = [&key](const std::vector<Card>& pile) {
        key.push_back('|');
        for (const Card& card : pile) {
            key += std::to_string(card.id);
            key.push_back(':');
            key += std::to_string(card.faceUp() ? card.value : card.knownValue);
            key.push_back(',');
        }
    };
    appendOrdered(state.talon);
    appendOrdered(state.waste);
    key += "|d" + std::to_string(state.drawCount);
    return key;
}

static int virtualMobility(const GameState& state) {
    int moves = 0;
    const auto countTableauTargets = [&](const Card& card) {
        for (int dst = 0; dst < 7; ++dst) {
            const Card* top = state.tableau[dst].empty() ? nullptr : &state.tableau[dst].back();
            if (canDropOnTableau(card, top)) ++moves;
        }
    };
    if (!state.waste.empty() && state.waste.back().faceUp()) countTableauTargets(state.waste.back());
    for (int src = 0; src < 7; ++src) {
        const int first = firstFaceUpIndex(state.tableau[src]);
        for (int start = first; start >= 0 && start < (int)state.tableau[src].size(); ++start) {
            const Card& card = state.tableau[src][start];
            if (!card.faceUp()) continue;
            for (int dst = 0; dst < 7; ++dst) {
                if (dst == src) continue;
                const Card* top = state.tableau[dst].empty() ? nullptr : &state.tableau[dst].back();
                if (canDropOnTableau(card, top)) ++moves;
            }
        }
    }
    return moves;
}

static int dependencyPenalty(const GameState& state) {
    std::array<int, 4> neededRank{0, 0, 0, 0};
    for (const auto& pile : state.foundations) {
        if (pile.empty()) continue;
        const int suit = knownCardSuit(pile.back());
        const int rank = knownCardRank(pile.back());
        if (suit >= 0 && suit < 4 && rank >= 0) neededRank[suit] = std::min(13, rank + 1);
    }

    int penalty = 0;
    for (int suit = 0; suit < 4; ++suit) {
        if (neededRank[suit] >= 13) continue;
        bool located = false;
        for (const auto& column : state.tableau) {
            for (int i = 0; i < (int)column.size(); ++i) {
                if (knownCardSuit(column[i]) != suit || knownCardRank(column[i]) != neededRank[suit]) continue;
                const int blockers = (int)column.size() - 1 - i;
                penalty += blockers * (neededRank[suit] <= 1 ? 240 : 100);
                if (!column[i].faceUp()) penalty += 220;
                located = true;
                break;
            }
            if (located) break;
        }
        if (located) continue;
        for (int i = (int)state.waste.size() - 1, distance = 0; i >= 0; --i, ++distance) {
            if (knownCardSuit(state.waste[i]) == suit && knownCardRank(state.waste[i]) == neededRank[suit]) {
                penalty += distance * 55;
                located = true;
                break;
            }
        }
        if (located) continue;
        // LocalGs stores the next stock card at index zero (confirmed against
        // consecutive real Draw-3 snapshots), while Waste stores its exposed
        // card at the back.  Distances must follow that asymmetric ordering.
        for (int i = 0, distance = 0; i < (int)state.talon.size(); ++i, ++distance) {
            if (knownCardSuit(state.talon[i]) == suit && knownCardRank(state.talon[i]) == neededRank[suit]) {
                const int draw = std::clamp(state.drawCount, 1, 3);
                penalty += (distance / draw) * 70 + (distance % draw) * 20;
                break;
            }
        }
    }
    return penalty;
}

static int virtualStructuralScore(const GameState& state) {
    int hidden = 0, exposed = 0, orderedLinks = 0;
    const int foundationCards = virtualFoundationCount(state);
    int blockedHighCards = 0, buriedCritical = 0;
    bool availableKing = false;
    for (const auto& column : state.tableau) {
        const int first = firstFaceUpIndex(column);
        if (first > 0 && first < (int)column.size()) {
            // A high card immediately above hidden cards is expensive: it is
            // commonly the card that prevents the column from being emptied.
            blockedHighCards += std::max(0, cardRank(column[first]) - 5);
        }
        for (int i = 0; first > 0 && i < first; ++i) {
            const int hiddenRank = knownCardRank(column[i]);
            if (hiddenRank >= 0 && (hiddenRank <= 1 || hiddenRank == 12))
                buriedCritical += first - i;
        }
        for (int i = 0; i < (int)column.size(); ++i) {
            if (!column[i].revealedForPlanning()) { ++hidden; continue; }
            ++exposed;
            // An opaque card that was virtually exposed proves progress but
            // has no rank/suit yet.  It cannot participate in another virtual
            // move until the physical snapshot resolves it.
            if (!column[i].faceUp()) continue;
            // A King already anchoring index zero cannot productively move to
            // another empty column. Any other exposed King can consume a newly
            // opened space and unlock the cards beneath its source.
            if (cardRank(column[i]) == 12 && i > 0) availableKing = true;
            if (i > 0 && column[i - 1].faceUp() &&
                canDropOnTableau(column[i], &column[i - 1])) ++orderedLinks;
        }
    }
    // The draw end is the next exposed card. Deep Aces, twos and Kings cost
    // future passes because they are strategically critical unlockers.
    for (int i = 0; i < (int)state.talon.size(); ++i) {
        const int rank = knownCardRank(state.talon[i]);
        const int distance = i;
        if (rank >= 0 && (rank <= 1 || rank == 12)) buriedCritical += distance + 1;
    }
    if (!state.waste.empty() && cardRank(state.waste.back()) == 12) availableKing = true;

    int emptyColumns = 0;
    for (const auto& column : state.tableau) if (column.empty()) ++emptyColumns;
    // An empty column is valuable only when a useful King can consume it.  A
    // vacant column without such a King is congestion in disguise: the bot
    // paid a move and reduced its construction surface for no immediate gain.
    const int emptyColumnValue = availableKing ? emptyColumns * 850 : -emptyColumns * 700;

    // Balanced foundations are safer than one suit racing far ahead.  This
    // encodes the same tactical reason as foundationAdvanceIsSafe in the global
    // evaluator, so a deep branch can trade a small imbalance for a reveal but
    // will not drift into a permanently trapped colour.
    std::array<int, 4> foundationRank{-1, -1, -1, -1};
    for (const auto& pile : state.foundations) {
        if (pile.empty()) continue;
        const int suit = knownCardSuit(pile.back());
        if (suit >= 0 && suit < 4) foundationRank[suit] = knownCardRank(pile.back());
    }
    const int minFoundation = *std::min_element(foundationRank.begin(), foundationRank.end());
    const int maxFoundation = *std::max_element(foundationRank.begin(), foundationRank.end());
    const int imbalancePenalty = std::max(0, maxFoundation - minFoundation - 2) * 360;
    const int foundationDebtPenalty = foundationRiskDebt(state.foundations) * 1800;

    return foundationCards * 260 + exposed * 32 + orderedLinks * 75 +
           virtualMobility(state) * 70 + emptyColumnValue - hidden * 2200 -
           blockedHighCards * 130 - buriedCritical * 14 - imbalancePenalty -
           foundationDebtPenalty - dependencyPenalty(state);
}

static bool applyVirtualMove(GameState& state, const Move& move) {
    if (move.kind == VirtualMoveKind::Talon) {
        if (!state.talon.empty()) {
            // Jawaker's Solo mode presents a three-card draw; preserving order
            // here lets planning evaluate future waste exposure accurately.
            const int draw = std::min(std::clamp(state.drawCount, 1, 3),
                                      (int)state.talon.size());
            for (int i = 0; i < draw; ++i) {
                Card drawn = state.talon.front();
                state.talon.erase(state.talon.begin());
                if (!drawn.faceUp() && drawn.known()) drawn.value = drawn.knownValue;
                state.waste.push_back(std::move(drawn));
            }
            return true;
        }
        if (state.waste.empty()) return false;
        // LocalGs keeps real values in talon/waste.  Preserve them so a full
        // recycle returns to the exact same native state key and the solver's
        // transposition table can prove the loop instead of inventing a new
        // face-down variant on every pass.
        for (Card& card : state.waste)
            if (card.faceUp()) card.knownValue = card.value;
        // Waste is oldest-to-newest and its back is exposed. Turning the pile
        // over makes the oldest remaining card the next stock card, exactly
        // the same front-to-back vector order; reversing here used to corrupt
        // every planned path after the first recycle.
        state.talon.swap(state.waste);
        ++state.recycles;
        return true;
    }
    if (move.kind == VirtualMoveKind::Foundation) {
        Card card;
        if (move.from < 0) {
            if (state.waste.empty()) return false;
            card = state.waste.back();
        } else {
            if (move.from >= 7 || state.tableau[move.from].empty()) return false;
            card = state.tableau[move.from].back();
        }
        if (move.to < 0 || move.to >= 4 || !card.faceUp()) return false;
        const Card* top = state.foundations[move.to].empty() ? nullptr : &state.foundations[move.to].back();
        if (!canDropOnFoundation(card, top)) return false;
        if (move.from < 0) {
            state.waste.pop_back();
        } else {
            state.tableau[move.from].pop_back();
            revealVirtualTop(state.tableau[move.from]);
        }
        state.foundations[move.to].push_back(card);
        return true;
    }
    if (move.kind == VirtualMoveKind::FoundationToTableau) {
        if (move.from < 0 || move.from >= 4 || move.to < 0 || move.to >= 7 ||
            state.foundations[move.from].empty()) return false;
        const Card card = state.foundations[move.from].back();
        const Card* top = state.tableau[move.to].empty() ? nullptr : &state.tableau[move.to].back();
        if (!canDropOnTableau(card, top)) return false;
        state.foundations[move.from].pop_back();
        state.tableau[move.to].push_back(card);
        return true;
    }
    if (move.to < 0 || move.to >= 7) return false;
    if (move.from < 0) {
        if (state.waste.empty() || !state.waste.back().faceUp()) return false;
        const Card& card = state.waste.back();
        const Card* top = state.tableau[move.to].empty() ? nullptr : &state.tableau[move.to].back();
        if (!canDropOnTableau(card, top)) return false;
        state.tableau[move.to].push_back(card);
        state.waste.pop_back();
        return true;
    }
    if (move.from >= 7) return false;
    const int start = move.start;
    if (start < 0 || start >= (int)state.tableau[move.from].size()) return false;
    const Card& card = state.tableau[move.from][start];
    if (!isStrategicTableauMove(state.tableau, move.from, start, move.to, card)) return false;
    std::vector<Card> sequence(state.tableau[move.from].begin() + start, state.tableau[move.from].end());
    state.tableau[move.from].erase(state.tableau[move.from].begin() + start, state.tableau[move.from].end());
    state.tableau[move.to].insert(state.tableau[move.to].end(), sequence.begin(), sequence.end());
    revealVirtualTop(state.tableau[move.from]);
    return true;
}

static void generateVirtualMoves(const GameState& state, std::vector<Move>& out,
                                 bool allowFoundationPullback = false) {
    out.clear();
    if (!state.waste.empty() && state.waste.back().faceUp()) {
        for (int f = 0; f < 4; ++f) {
            const Card* top = state.foundations[f].empty() ? nullptr : &state.foundations[f].back();
            if (canDropOnFoundation(state.waste.back(), top))
                out.push_back({VirtualMoveKind::Foundation, -1, f, -1});
        }
        for (int dst = 0; dst < 7; ++dst) {
            const Card* top = state.tableau[dst].empty() ? nullptr : &state.tableau[dst].back();
            if (canDropOnTableau(state.waste.back(), top))
                out.push_back({VirtualMoveKind::Tableau, -1, dst, -1});
        }
    }
    for (int src = 0; src < 7; ++src) {
        if (!state.tableau[src].empty() && state.tableau[src].back().faceUp()) {
            for (int f = 0; f < 4; ++f) {
                const Card* top = state.foundations[f].empty() ? nullptr : &state.foundations[f].back();
                const Card& card = state.tableau[src].back();
                if (canDropOnFoundation(card, top))
                    out.push_back({VirtualMoveKind::Foundation, src, f, -1});
            }
        }
        const int first = firstFaceUpIndex(state.tableau[src]);
        for (int start = first; start >= 0 && start < (int)state.tableau[src].size(); ++start) {
            const Card& card = state.tableau[src][start];
            for (int dst = 0; dst < 7; ++dst) {
                if (!isStrategicTableauMove(state.tableau, src, start, dst, card)) continue;
                const bool clearsSource = start == 0;
                if (clearsSource && cardRank(card) != 12 &&
                    !hasReachableKingForNewColumn(state, src)) continue;
                    out.push_back({VirtualMoveKind::Tableau, src, dst, start});
            }
        }
    }
    // Foundation pullback is excluded from ordinary play. With complete
    // information it is admitted only by strategic recovery, where a searched
    // continuation must repay this temporary regression. This preserves the
    // strong no-Undo baseline while keeping the one legal escape that can
    // reopen a blocked Tableau in a genuinely recoverable position.
    if (allowFoundationPullback) {
        for (int src = 0; src < 4; ++src) {
            if (state.foundations[src].empty()) continue;
            const Card& card = state.foundations[src].back();
            if (!card.faceUp() || cardRank(card) < 2) continue; // never pull A/2
            for (int dst = 0; dst < 7; ++dst) {
                if (state.tableau[dst].empty()) continue;
                const Card* top = &state.tableau[dst].back();
                if (canDropOnTableau(card, top))
                    out.push_back({VirtualMoveKind::FoundationToTableau, src, dst, -1});
            }
        }
    }
    // Stock is a normal branch, not an unconditional fallback, which permits
    // the planner to intentionally defer a currently legal waste move.
    if (!state.talon.empty() || !state.waste.empty())
        out.push_back({VirtualMoveKind::Talon, -1, -1, -1});
}

struct PlannerSearchContext {
    int budget = 0;
    uint64_t deadlineMs = 0;
    std::unordered_map<std::string, int> transposition;
    std::unordered_set<std::string> path;
    bool allowFoundationPullback = false;
};

static bool seenRecentSolverState(const std::string& key, uint64_t now) {
    for (auto it = g_solverStateHistory.rbegin(); it != g_solverStateHistory.rend(); ++it) {
        if (now - it->at > 180000) break;
        if (it->tableau == key) return true;
    }
    return false;
}

static int searchVirtualFuture(const GameState& state, int depth,
                               PlannerSearchContext& context) {
    const int base = virtualStructuralScore(state);
    if (virtualFoundationCount(state) == 52) return base + 1000000;
    if (depth <= 0 || context.budget <= 0 || nowMs() >= context.deadlineMs) return base;

    const std::string stateKey = solverPositionKey(state);
    if (context.path.find(stateKey) != context.path.end()) return -8000000;
    const std::string cacheKey = stateKey + '#' + std::to_string(depth);
    const auto cached = context.transposition.find(cacheKey);
    if (cached != context.transposition.end()) return cached->second;
    context.path.insert(stateKey);

    std::vector<Move> moves;
    generateVirtualMoves(state, moves, context.allowFoundationPullback);
    if (moves.empty()) {
        context.path.erase(stateKey);
        return hasOpaqueCards(state) ? base - 5000 : -10000000;
    }

    struct Branch { GameState state; int heuristic; std::string key; };
    std::vector<Branch> branches;
    branches.reserve(moves.size());
    for (const Move& move : moves) {
        if (context.budget-- <= 0 || nowMs() >= context.deadlineMs) break;
        GameState next = state;
        if (!applyVirtualMove(next, move)) continue;
        std::string key = solverPositionKey(next);
        if (context.path.find(key) != context.path.end()) continue;
        branches.push_back({std::move(next), 0, std::move(key)});
        branches.back().heuristic = virtualStructuralScore(branches.back().state);
    }
    if (branches.empty()) {
        context.path.erase(stateKey);
        return hasOpaqueCards(state) ? base - 5000 : -10000000;
    }
    std::sort(branches.begin(), branches.end(), [](const Branch& a, const Branch& b) { return a.heuristic > b.heuristic; });
    int hidden = 0;
    for (const auto& column : state.tableau)
        for (const Card& card : column) if (!card.faceUp()) ++hidden;
    // Keep the established fast beam in the open game.  Near the end, where
    // the state space is much smaller and one temporarily unattractive move
    // can be the only winning bridge, retain more siblings instead of letting
    // the short-term heuristic kill them.  The existing node/time budget is
    // unchanged, so this cannot turn the Unity thread into an unbounded solve.
    const int requestedBeam = hidden <= 4 ? 12 : (hidden <= 9 ? 9 : 6);
    const int beamWidth = std::min(requestedBeam, (int)branches.size());
    int best = -10000000;
    for (int i = 0; i < beamWidth && context.budget > 0 && nowMs() < context.deadlineMs; ++i)
        best = std::max(best, searchVirtualFuture(branches[i].state, depth - 1, context));
    context.path.erase(stateKey);
    context.transposition.emplace(cacheKey, best);
    return best;
}

static int candidateTacticalBonus(const GameState& before, const GameState& after,
                                  const Move& move) {
    int bonus = 0;
    int hiddenBefore = 0, hiddenAfter = 0;
    for (const auto& pile : before.tableau) for (const Card& c : pile) if (!c.faceUp()) ++hiddenBefore;
    for (const auto& pile : after.tableau) for (const Card& c : pile) if (!c.faceUp()) ++hiddenAfter;
    bonus += (hiddenBefore - hiddenAfter) * 12000; // reveal is the primary Klondike objective.
    if (move.kind == VirtualMoveKind::Foundation) {
        const Card* card = nullptr;
        if (move.from < 0) {
            if (!before.waste.empty()) card = &before.waste.back();
        } else if (move.from < 7 && !before.tableau[move.from].empty()) {
            card = &before.tableau[move.from].back();
        }
        const int debtDelta = foundationRiskDebt(after.foundations) -
                              foundationRiskDebt(before.foundations);
        if (card && foundationAdvanceIsSafe(*card, before.foundations) && debtDelta <= 0) {
            bonus += hiddenBefore == 0 ? 3600 : 1200;
        } else {
            // Risky pushes remain searchable because occasionally a blocker
            // really must move.  The large debt cost prevents executing that
            // push as a short-sighted win; a later reveal/rebalance in the same
            // branch has to earn it back first.
            bonus -= (move.from < 0 ? 6000 : 9000) + std::max(0, debtDelta) * 3500;
        }
    }
    if (move.kind == VirtualMoveKind::FoundationToTableau) bonus -= 9000;
    // Exact talon/waste state hashing now catches full stock cycles, so this is
    // only a small path-length preference.  Necessary Draw-1/Draw-3 access to
    // a known buried card must not lose against an unrelated shallow move.
    if (move.kind == VirtualMoveKind::Talon) bonus -= 70;
    if (move.kind == VirtualMoveKind::Tableau && move.from >= 0 &&
        before.tableau[move.to].empty()) bonus += 450;
    return bonus;
}

static int scoreFutureCandidate(const GameState& current, const Move& candidate,
                                int deepSteps, int budget, uint64_t deadlineMs,
                                bool allowFoundationPullback = false) {
    GameState after = current;
    if (!applyVirtualMove(after, candidate)) return -10000000;
    const std::string resultKey = virtualPositionKey(after);
    int repetitionPenalty = seenRecentSolverState(resultKey, nowMs()) ? 4000000 : 0;
    PlannerSearchContext context;
    context.budget = budget;
    context.deadlineMs = deadlineMs;
    context.allowFoundationPullback = allowFoundationPullback;
    context.transposition.reserve((size_t)budget);
    context.path.reserve(64);
    const int future = searchVirtualFuture(after, std::clamp(deepSteps, 15, 20), context);
    if (future <= -8000000) return future;
    return future + candidateTacticalBonus(current, after, candidate) - repetitionPenalty;
}

static bool simulate_future_moves(GameState current, Move candidate_move, int deep_steps) {
    return scoreFutureCandidate(current, candidate_move, deep_steps, 720, nowMs() + 18) > -8000000;
}

static GameState makeGameState(const std::array<std::vector<Card>, 7>& tableau,
                               const std::array<std::vector<Card>, 4>& foundations,
                               const std::vector<Card>& talon,
                               const std::vector<Card>& waste) {
    GameState state;
    state.tableau = tableau;
    state.foundations = foundations;
    state.talon = talon;
    state.waste = waste;
    state.recycles = g_actualRecycleCount;
    state.drawCount = g_cardsPerTalonFlip;
    return state;
}

struct RescuePlan {
    bool valid = false;
    int from = -1;
    int to = -1;
    int cardId = -1;
    int score = -1000000;
};

static int tableauSearchScore(const std::array<std::vector<Card>, 7>& tableau,
                              const std::array<std::vector<Card>, 4>& foundations) {
    int hidden = 0;
    int exposed = 0;
    int empty = 0;
    bool kingAvailable = false;
    for (const auto& column : tableau) {
        if (column.empty()) { ++empty; continue; }
        for (const Card& card : column) {
            if (card.faceUp()) {
                ++exposed;
                if (cardRank(card) == 12) kingAvailable = true;
            } else {
                ++hidden;
            }
        }
    }
    const int usefulEmpty = kingAvailable ? empty : 0;
    return -hidden * 10000 + countProductiveOptions(tableau, foundations) * 1000 +
           exposed * 20 + usefulEmpty * 300;
}

static int searchTableauDepth(const std::array<std::vector<Card>, 7>& tableau,
                              const std::array<std::vector<Card>, 4>& foundations,
                              int depth, int& budget, int lastFrom, int lastTo, int lastCardId) {
    int best = tableauSearchScore(tableau, foundations);
    if (depth <= 0 || budget <= 0) return best;
    for (int from = 0; from < 7 && budget > 0; ++from) {
        const int first = firstFaceUpIndex(tableau[from]);
        if (first < 0) continue;
        for (int start = first; start < (int)tableau[from].size() && budget > 0; ++start) {
            const Card& moving = tableau[from][start];
            if (!moving.faceUp()) continue;
            for (int to = 0; to < 7 && budget > 0; ++to) {
                if (from == to) continue;
                if (moving.id == lastCardId && from == lastTo && to == lastFrom) continue;
                if (!isStrategicTableauMove(tableau, from, start, to, moving)) continue;
                --budget;
                auto next = tableau;
                std::vector<Card> sequence(next[from].begin() + start, next[from].end());
                next[from].erase(next[from].begin() + start, next[from].end());
                next[to].insert(next[to].end(), sequence.begin(), sequence.end());
                best = std::max(best, searchTableauDepth(next, foundations, depth - 1,
                                                         budget, from, to, moving.id));
            }
        }
    }
    return best;
}

static bool findRescuePlan(const std::array<std::vector<Card>, 7>& tableau,
                           const std::array<std::vector<Card>, 4>& foundations,
                           RescuePlan& out) {
    const int baseline = tableauSearchScore(tableau, foundations);
    int budget = 384; // bounded deep search; keeps Unity responsive.
    for (int from = 0; from < 7 && budget > 0; ++from) {
        const int first = firstFaceUpIndex(tableau[from]);
        if (first < 0) continue;
        for (int start = first; start < (int)tableau[from].size() && budget > 0; ++start) {
            const Card& moving = tableau[from][start];
            if (!moving.faceUp()) continue;
            for (int to = 0; to < 7 && budget > 0; ++to) {
                if (from == to) continue;
                if (!isStrategicTableauMove(tableau, from, start, to, moving)) continue;
                --budget;
                auto next = tableau;
                std::vector<Card> sequence(next[from].begin() + start, next[from].end());
                next[from].erase(next[from].begin() + start, next[from].end());
                next[to].insert(next[to].end(), sequence.begin(), sequence.end());
                const int score = searchTableauDepth(next, foundations, 4, budget,
                                                     from, to, moving.id);
                if ((!out.valid || score > out.score) && score > baseline) {
                    out = {true, from, to, moving.id, score};
                }
            }
        }
    }
    return out.valid;
}

static int nextUsefulTalonDistance(const std::vector<Card>& talon,
                                   const std::array<std::vector<Card>, 7>& tableau,
                                   const std::array<std::vector<Card>, 4>& foundations) {
    for (int i = (int)talon.size() - 1, distance = 0; i >= 0; --i, ++distance) {
        const Card& card = talon[i];
        if (!card.faceUp()) continue;
        for (int f = 0; f < 4; ++f) {
            const Card* top = foundations[f].empty() ? nullptr : &foundations[f].back();
            if (canDropOnFoundation(card, top) && foundationAdvanceIsSafe(card, foundations)) return distance;
        }
        for (int dst = 0; dst < 7; ++dst) {
            const Card* top = tableau[dst].empty() ? nullptr : &tableau[dst].back();
            if (canDropOnTableau(card, top)) return distance;
        }
    }
    return -1;
}

static void* findStackList(void* localGs, const std::string& name) {
    if (!localGs) return nullptr;
    void* dict = *(void**)((uint8_t*)localGs + OFF_SOL_STACKS);
    if (!dict) return nullptr;
    const int count = *(int*)((uint8_t*)dict + 0x20);
    void* entries = *(void**)((uint8_t*)dict + 0x18);
    if (!entries || count < 0 || count > 64) return nullptr;
    for (int i = 0; i < count; ++i) {
        const uint8_t* e = (const uint8_t*)entries + OFF_ARRAY_ITEMS + i * 24;
        if (*(const int*)e < 0) continue;
        if (readIl2CppString(*(void* const*)(e + 8)) == name) {
            return *(void* const*)(e + 16);
        }
    }
    return nullptr;
}

static void logCardRaw(const char* stack, int index, void* item) {
    void* values = item ? *(void**)((uint8_t*)item + OFF_JARRAY_VALUES) : nullptr;
    const int valueCount = listSize(values);
    void* first = values ? listItem(values, 0) : nullptr;
    void* second = values ? listItem(values, 1) : nullptr;
    void* third = values ? listItem(values, 2) : nullptr;
    const void* firstBox = first ? *(void* const*)((uint8_t*)first + OFF_JVALUE_VALUE) : nullptr;
    const void* secondBox = second ? *(void* const*)((uint8_t*)second + OFF_JVALUE_VALUE) : nullptr;
    const void* thirdBox = third ? *(void* const*)((uint8_t*)third + OFF_JVALUE_VALUE) : nullptr;
    const long long firstRaw = firstBox ? *(const long long*)((const uint8_t*)firstBox + 0x10) : -1;
    const long long secondRaw = secondBox ? *(const long long*)((const uint8_t*)secondBox + 0x10) : -1;
    Card parsed = parseCard(item);
    std::string props;
    void* collection = item ? *(void**)((uint8_t*)item + OFF_JOBJECT_PROPERTIES) : nullptr;
    void* propertyItems = collection ? *(void**)((uint8_t*)collection + OFF_COLLECTION_ITEMS) : nullptr;
    const int propertyCount = listSize(propertyItems);
    for (int i = 0; i < propertyCount; ++i) {
        void* property = listItem(propertyItems, i);
        if (!property) continue;
        const std::string key = readIl2CppString(*(void**)((uint8_t*)property + OFF_JPROPERTY_NAME));
        void* content = *(void**)((uint8_t*)property + OFF_JPROPERTY_CONTENT);
        void* value = content ? *(void**)((uint8_t*)content + OFF_JPROPERTYLIST_TOKEN) : nullptr;
        const int type = value ? *(int*)((uint8_t*)value + 0x30) : -1;
        void* box = value ? *(void**)((uint8_t*)value + OFF_JVALUE_VALUE) : nullptr;
        const long long raw = box ? *(const long long*)((uint8_t*)box + 0x10) : -1;
        if (!props.empty()) props += ";";
        props += key + "=" + std::to_string(raw) + ":t" + std::to_string(type);
        if (type == 8) props += ":" + tokenString(value);
    }
    void* exc = nullptr;
    void* serial = (item && g_objectToString) ? g_objectToString(item, &exc) : nullptr;
    const char* className = (item && g_classGetName) ? g_classGetName(*(void**)item) : "?";
    const std::string json = (!exc && serial) ? readIl2CppString(serial) : "<exception>";
    SOL_LOG("snapshot stack=%s idx=%d item=%p class=%s values=%p n=%d fields=[%p:t%d:box%p:v%lld,%p:t%d:box%p:v%lld,%p:t%d:box%p] props=%s parsed=id%d val%d color=%s json=%s",
            stack, index, item, className ? className : "?", values, valueCount,
            first, first ? *(int*)((uint8_t*)first + 0x30) : -1, firstBox, firstRaw,
            second, second ? *(int*)((uint8_t*)second + 0x30) : -1, secondBox, secondRaw,
            third, third ? *(int*)((uint8_t*)third + 0x30) : -1, thirdBox, props.c_str(),
            parsed.id, parsed.value, parsed.color.c_str(), json.c_str());
}

static void logFullSnapshot(void* localGs) {
    if (!localGs || g_loggedSnapshot) return;
    g_loggedSnapshot = true;
    SOL_LOG("snapshot begin prefix=%s", g_stackPrefix.c_str());
    for (int i = 0; i < 7; ++i) {
        const std::string name = g_stackPrefix + "tableau_" + std::to_string(i);
        void* list = findStackList(localGs, name);
        const int count = listSize(list);
        SOL_LOG("snapshot list=%s ptr=%p count=%d", name.c_str(), list, count);
        for (int j = 0; j < count; ++j) logCardRaw(name.c_str(), j, listItem(list, j));
    }
    for (const char* suffix : {"waste", "talon", "foundation_0", "foundation_1", "foundation_2", "foundation_3"}) {
        const std::string name = g_stackPrefix + suffix;
        void* list = findStackList(localGs, name);
        const int count = listSize(list);
        SOL_LOG("snapshot list=%s ptr=%p count=%d", name.c_str(), list, count);
        for (int j = 0; j < count; ++j) logCardRaw(name.c_str(), j, listItem(list, j));
    }
    SOL_LOG("snapshot end");
}

static std::vector<Card> stackCards(void* localGs, const std::string& name) {
    std::vector<Card> out;
    if (!localGs) return out;
    void* list = findStackList(localGs, name);
    const int count = listSize(list);
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        Card c = parseCard(listItem(list, i));
        if (c.valid()) out.push_back(std::move(c));
    }
    return out;
}

static void discoverStackPrefix(void* localGs) {
    if (!localGs) return;
    void* dict = *(void**)((uint8_t*)localGs + OFF_SOL_STACKS);
    if (!dict) return;
    const int count = *(int*)((uint8_t*)dict + 0x20);
    void* entries = *(void**)((uint8_t*)dict + 0x18);
    if (!entries || count < 0 || count > 64) return;
    for (int i = 0; i < count; ++i) {
        const uint8_t* e = (const uint8_t*)entries + OFF_ARRAY_ITEMS + i * 24;
        if (*(const int*)e < 0) continue;
        const std::string key = readIl2CppString(*(void* const*)(e + 8));
        const size_t pos = key.find("tableau_0");
        if (pos != std::string::npos) {
            const std::string prefix = key.substr(0, pos);
            if (!prefix.empty() && prefix != g_stackPrefix) {
                g_stackPrefix = prefix;
                SOL_LOG("using stack prefix=%s", g_stackPrefix.c_str());
            }
            return;
        }
    }
}

static void logNativeStackKeys(void* localGs) {
    if (!localGs || g_loggedStackKeys) return;
    void* dict = *(void**)((uint8_t*)localGs + OFF_SOL_STACKS);
    if (!dict) return;
    // Dictionary<TKey,TValue>: entries at +0x18, count at +0x20.  An entry
    // with reference-type key/value is {hash,next,key,value}, 24 bytes.
    const int count = *(int*)((uint8_t*)dict + 0x20);
    void* entries = *(void**)((uint8_t*)dict + 0x18);
    if (!entries || count < 0 || count > 64) return;
    std::string keys;
    for (int i = 0; i < count; ++i) {
        const uint8_t* e = (const uint8_t*)entries + OFF_ARRAY_ITEMS + i * 24;
        const int hash = *(const int*)e;
        if (hash < 0) continue;
        std::string key = readIl2CppString(*(void* const*)(e + 8));
        if (key.empty()) continue;
        if (!keys.empty()) keys += ",";
        keys += key;
    }
    if (!keys.empty()) {
        g_loggedStackKeys = true;
        SOL_LOG("native stack keys=%s", keys.c_str());
    }
}

static void* findRaycastController() {
    if (g_rayController) return g_rayController;
    if (!g_domainGet || !g_domainAssemblies || !g_assemblyImage || !g_imageName ||
        !g_classFromName || !g_staticData) return nullptr;
    void* domain = g_domainGet();
    size_t count = 0;
    const void** assemblies = domain ? g_domainAssemblies(domain, &count) : nullptr;
    if (!assemblies || count > 256) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = g_assemblyImage(assemblies[i]);
        const char* imageName = image ? g_imageName(image) : nullptr;
        if (!imageName || std::string(imageName) != "Assembly-CSharp.dll") continue;
        void* klass = g_classFromName(image, "Jawaker", "SolitaireRaycastController");
        void* statics = klass ? g_staticData(klass) : nullptr;
        void* instance = statics ? *(void**)statics : nullptr; // static Ins is the first field
        if (instance) {
            g_rayController = instance;
            SOL_LOG("raycast controller=%p", g_rayController);
        }
        return g_rayController;
    }
    return nullptr;
}

static void* findCardsController() {
    if (g_cardsController) return g_cardsController;
    if (!g_domainGet || !g_domainAssemblies || !g_assemblyImage || !g_imageName ||
        !g_classFromName || !g_staticData) return nullptr;
    void* domain = g_domainGet();
    size_t count = 0;
    const void** assemblies = domain ? g_domainAssemblies(domain, &count) : nullptr;
    if (!assemblies || count > 256) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = g_assemblyImage(assemblies[i]);
        const char* imageName = image ? g_imageName(image) : nullptr;
        if (!imageName || std::string(imageName) != "Assembly-CSharp.dll") continue;
        void* klass = g_classFromName(image, "Jawaker", "CardsController");
        void* statics = klass ? g_staticData(klass) : nullptr;
        // CardsController.ins is its first and only relevant static field.
        g_cardsController = statics ? *(void**)statics : nullptr;
        if (g_cardsController) SOL_LOG("cards controller=%p", g_cardsController);
        return g_cardsController;
    }
    return nullptr;
}

static void* currentRawGameStoreState() {
    if (!g_gameStoreStatics) {
        if (!g_domainGet || !g_domainAssemblies || !g_assemblyImage ||
            !g_imageName || !g_classFromName || !g_staticData) return nullptr;
        void* domain = g_domainGet();
        size_t count = 0;
        const void** assemblies = domain ? g_domainAssemblies(domain, &count) : nullptr;
        if (!assemblies || count > 256) return nullptr;
        for (size_t i = 0; i < count; ++i) {
            void* image = g_assemblyImage(assemblies[i]);
            const char* imageName = image ? g_imageName(image) : nullptr;
            if (!imageName || std::string(imageName) != "Assembly-CSharp.dll")
                continue;
            void* klass = g_classFromName(image, "Jawaker.Games.Core", "GameStore");
            g_gameStoreStatics = klass ? g_staticData(klass) : nullptr;
            break;
        }
    }
    return g_gameStoreStatics
        ? *(void**)((uint8_t*)g_gameStoreStatics + OFF_GAMESTORE_STATIC_GS)
        : nullptr;
}

static void* findCardController(void* rayController, int cardId) {
    if (!rayController || cardId < 0) return nullptr;
    // SolitaireRaycastController._cardsPile is normally populated by the
    // multiplayer scene. Solo Solitaire leaves it empty, so use the game's
    // shared CardsController.allCards list (0x60) as the authoritative view.
    void* cards = *(void**)((uint8_t*)rayController + 0xA0);
    int count = listSize(cards);
    if (count == 0) {
        void* cardsController = findCardsController();
        cards = cardsController ? *(void**)((uint8_t*)cardsController + 0x60) : nullptr;
        count = listSize(cards);
    }
    void* result = nullptr;
    std::string visible;
    for (int i = 0; i < count; ++i) {
        void* controller = listItem(cards, i);
        if (!controller) continue;
        const int id = *(int*)((uint8_t*)controller + 0x4C);
        const int value = *(int*)((uint8_t*)controller + 0x50);
        if (i < 18) {
            if (!visible.empty()) visible += ',';
            visible += std::to_string(id) + ":" + std::to_string(value);
        }
        if (id == cardId) result = controller;
    }
    if (!result) {
        const uint64_t now = nowMs();
        if (now - g_lastCardDiagMs > 1800) {
            g_lastCardDiagMs = now;
            SOL_LOG("card-view miss wanted=%d pile=%p count=%d visible=[%s]", cardId,
                    cards, count, visible.c_str());
        }
    }
    return result;
}

static int activeSolitairePlayerIndex() {
    const size_t prefix = g_stackPrefix.find("p_");
    if (prefix == std::string::npos) return -1;
    int value = 0;
    bool found = false;
    for (size_t i = prefix + 2; i < g_stackPrefix.size(); ++i) {
        const char c = g_stackPrefix[i];
        if (c < '0' || c > '9') break;
        found = true;
        value = value * 10 + (c - '0');
    }
    return found ? value : -1;
}

static void hydratePerfectInformation(void* localGs,
                                      std::array<std::vector<Card>, 7>& tableau,
                                      std::vector<Card>& talon,
                                      std::vector<Card>& waste,
                                      std::array<std::vector<Card>, 4>& foundations) {
    std::unordered_map<int, int> observed;
    observed.reserve(52);
    const auto observe = [&observed](const std::vector<Card>& pile) {
        for (const Card& card : pile)
            if (card.id >= 0 && card.faceUp()) observed[card.id] = card.value;
    };
    for (const auto& column : tableau) observe(column);
    for (const auto& pile : foundations) observe(pile);
    observe(talon);
    observe(waste);

    // Once a complete deal has passed the 52-card bijection and live-visible
    // checks, it is immutable for this LocalGs round. Hydrate directly from the
    // compact array on subsequent ticks instead of walking IL2CPP dictionaries,
    // JSON objects and card controllers every frame. This is the hot-path speed
    // win: perfect information stays identical while per-move allocation and
    // reflection overhead disappears.
    const auto hydrateFromRoundCache = [](std::vector<Card>& pile) {
        for (Card& card : pile) {
            if (card.id >= 0 && card.id < 52) {
                const int value = g_roundCardValues[(size_t)card.id];
                if (value >= 0 && value < 52) card.knownValue = value;
                else if (card.faceUp()) card.knownValue = card.value;
            } else if (card.faceUp()) {
                card.knownValue = card.value;
            }
        }
    };
    if (g_mappingValidated) {
        bool visibleStillMatches = true;
        for (const auto& visible : observed) {
            if (visible.first < 0 || visible.first >= 52 ||
                g_roundCardValues[(size_t)visible.first] != visible.second) {
                visibleStillMatches = false;
                break;
            }
        }
        if (visibleStillMatches) {
            for (auto& column : tableau) hydrateFromRoundCache(column);
            for (auto& pile : foundations) hydrateFromRoundCache(pile);
            hydrateFromRoundCache(talon);
            hydrateFromRoundCache(waste);
            return;
        }

        // A visible conflict means Unity reused an object unexpectedly. Never
        // keep driving the solver with stale perfect information: retain only
        // values proven by the physical board and rebuild the deal mapping.
        g_mappingValidated = false;
        for (int id = 0; id < 52; ++id) {
            const auto it = observed.find(id);
            g_roundCardValues[(size_t)id] =
                it == observed.end() ? -1 : it->second;
        }
        SOL_LOG("perfect cache invalidated reason=visible-conflict");
    }

    struct MappingCandidate {
        std::vector<std::pair<int, int>> entries;
        int player = -1;
        int directMatches = 0;
        int directConflicts = 0;
        int reverseMatches = 0;
        bool reverse = false;
        bool complete = false;
        bool fromGameStore = false;
    };
    int players = 0;
    const int preferredPlayer = activeSolitairePlayerIndex();
    const auto isCompleteDirectMapping = [&observed](MappingCandidate& candidate) {
        if (candidate.entries.size() != 52 || candidate.directConflicts != 0)
            return false;
        std::array<bool, 52> seenIds{};
        std::array<bool, 52> seenValues{};
        for (const auto& entry : candidate.entries) {
            const int id = entry.first;
            const int value = entry.second;
            if (id < 0 || id >= 52 || value < 0 || value >= 52 ||
                seenIds[(size_t)id] || seenValues[(size_t)value])
                return false;
            seenIds[(size_t)id] = true;
            seenValues[(size_t)value] = true;
        }
        // A stale mapping from a previous deal (or another player) can still
        // be a 52-card permutation. Every physically visible card must agree.
        for (const auto& visible : observed) {
            bool matched = false;
            for (const auto& mapped : candidate.entries) {
                if (mapped.first == visible.first) {
                    matched = mapped.second == visible.second;
                    break;
                }
            }
            if (!matched) return false;
        }
        return true;
    };
    const auto mappingScore = [preferredPlayer](const MappingCandidate& candidate) {
        return (candidate.complete ? 1000000 : 0) +
               candidate.directMatches * 32 -
               candidate.directConflicts * 10000 +
               (int)candidate.entries.size() * 2 +
               (candidate.player == preferredPlayer ? 16 : 0);
    };

    const auto readBestMapping = [&](int& playerCount) {
        MappingCandidate best;
        void* mappings = localGs
            ? *(void**)((uint8_t*)localGs + OFF_SOL_PLAYER_CARDS_MAPPING)
            : nullptr;
        if (!mappings) return best;
        const size_t length = *(size_t*)((uint8_t*)mappings + 0x18);
        if (length == 0 || length > 8) return best;
        playerCount = (int)length;
        int bestScore = -100000000;
        for (int player = 0; player < playerCount; ++player) {
            void* dictionary = *(void**)((uint8_t*)mappings + OFF_ARRAY_ITEMS +
                                         (size_t)player * sizeof(void*));
            if (!dictionary) continue;
            void* entriesArray = *(void**)((uint8_t*)dictionary + 0x18);
            const int count = *(int*)((uint8_t*)dictionary + 0x20);
            if (!entriesArray || count <= 0 || count > 128) continue;
            const size_t capacity = *(size_t*)((uint8_t*)entriesArray + 0x18);
            if (capacity == 0 || capacity > 128) continue;

            MappingCandidate candidate;
            candidate.player = player;
            candidate.entries.reserve((size_t)count);
            const int scan = std::min(count, (int)capacity);
            const uint8_t* base = (const uint8_t*)entriesArray + OFF_ARRAY_ITEMS;
            for (int i = 0; i < scan; ++i) {
                // IDA ground truth: SyncPlayerCardsMapping stores cardId as
                // the Dictionary key and cardValue as its value. No heuristic
                // key/value reversal is permitted here.
                const int* entry = (const int*)(base + (size_t)i * 16);
                if (entry[0] < 0 || entry[2] < 0 || entry[2] >= 52 ||
                    entry[3] < 0 || entry[3] >= 52) continue;
                candidate.entries.emplace_back(entry[2], entry[3]);
                const auto visible = observed.find(entry[2]);
                if (visible != observed.end()) {
                    if (visible->second == entry[3]) ++candidate.directMatches;
                    else ++candidate.directConflicts;
                }
            }
            candidate.complete = isCompleteDirectMapping(candidate);
            const int candidateScore = mappingScore(candidate);
            if (!candidate.entries.empty() && candidateScore > bestScore) {
                bestScore = candidateScore;
                best = std::move(candidate);
            }
        }
        return best;
    };

    const auto readBestRawGameStoreMapping = [&](int& playerCount) {
        MappingCandidate best;
        void* rawGs = currentRawGameStoreState();
        void* mappingToken = rawGs
            ? objectFieldAny(rawGs, {"player_cards_mapping", "playerCardsMapping"})
            : nullptr;
        void* playerArrays = mappingToken
            ? *(void**)((uint8_t*)mappingToken + OFF_JARRAY_VALUES)
            : nullptr;
        const int count = listSize(playerArrays);
        if (count <= 0 || count > 8) return best;
        playerCount = count;
        int bestScore = -100000000;
        for (int player = 0; player < count; ++player) {
            void* playerArray = listItem(playerArrays, player);
            void* values = playerArray
                ? *(void**)((uint8_t*)playerArray + OFF_JARRAY_VALUES)
                : nullptr;
            const int valueCount = listSize(values);
            if (valueCount <= 0 || valueCount > 52) continue;

            MappingCandidate candidate;
            candidate.player = player;
            candidate.fromGameStore = true;
            candidate.entries.reserve((size_t)valueCount);
            for (int id = 0; id < valueCount; ++id) {
                const int value = tokenInt(listItem(values, id));
                if (value < 0 || value >= 52) continue;
                candidate.entries.emplace_back(id, value);
                const auto visible = observed.find(id);
                if (visible != observed.end()) {
                    if (visible->second == value) ++candidate.directMatches;
                    else ++candidate.directConflicts;
                }
            }
            candidate.complete = isCompleteDirectMapping(candidate);
            const int candidateScore = mappingScore(candidate);
            if (!candidate.entries.empty() && candidateScore > bestScore) {
                bestScore = candidateScore;
                best = std::move(candidate);
            }
        }
        return best;
    };

    MappingCandidate best = readBestMapping(players);
    // Frida/IDA verification against the live 28.2.90 build showed that the
    // raw GameStore can already hold the opponent's complete 52-card mapping
    // while the local player's mapping intentionally keeps tableau cards at
    // -1. Both players use the same deal order. Read that raw tree only until
    // a full mapping is validated, and reject it unless every visible card
    // agrees and the result is an exact 0..51 bijection.
    if (!best.complete && !g_mappingValidated) {
        int rawPlayers = 0;
        MappingCandidate raw = readBestRawGameStoreMapping(rawPlayers);
        if (!raw.entries.empty() &&
            (best.entries.empty() || mappingScore(raw) > mappingScore(best))) {
            best = std::move(raw);
            players = rawPlayers;
        }
    }

    // The UI can publish all 52 stacks one frame before it calls the native
    // LocalGs mapping sync. Ask the game's own narrow sync routine to refresh
    // only this field. Retries are round-bounded and cease permanently once a
    // validated 52/52 permutation is obtained, so normal move pacing and the
    // expert search budgets remain bit-for-bit unchanged.
    int physicalCards = (int)talon.size() + (int)waste.size();
    for (const auto& column : tableau) physicalCards += (int)column.size();
    for (const auto& pile : foundations) physicalCards += (int)pile.size();
    const uint64_t mappingNow = nowMs();
    if (!best.complete && !g_mappingValidated && physicalCards == 52 &&
        g_syncPlayerCardsMapping &&
        g_mappingSyncAttempts < SOL_MAPPING_SYNC_MAX_ATTEMPTS &&
        (g_lastMappingSyncMs == 0 ||
         mappingNow - g_lastMappingSyncMs >= SOL_MAPPING_SYNC_RETRY_MS)) {
        g_lastMappingSyncMs = mappingNow;
        ++g_mappingSyncAttempts;
        const bool changed = g_syncPlayerCardsMapping(localGs);
        players = 0;
        best = readBestMapping(players);
        if (g_mappingSyncAttempts == 1 || best.complete ||
            g_mappingSyncAttempts == SOL_MAPPING_SYNC_MAX_ATTEMPTS) {
            SOL_LOG("perfect native-sync attempt=%d changed=%d player=%d pairs=%d matches=%d conflicts=%d full=%d",
                    g_mappingSyncAttempts, changed ? 1 : 0, best.player,
                    (int)best.entries.size(), best.directMatches,
                    best.directConflicts, best.complete ? 1 : 0);
        }
    }

    // Preserve every authoritative value already seen in this round.  The
    // game's PlayerCardsMapping is allowed to be empty for several snapshots
    // (and, in some bot rounds, for the whole deal), so rebuilding this map
    // from scratch each tick silently discarded useful perfect information.
    for (const auto& entry : observed) {
        if (entry.first >= 0 && entry.first < 52 &&
            entry.second >= 0 && entry.second < 52)
            g_roundCardValues[(size_t)entry.first] = entry.second;
    }
    if (best.complete) {
        // A complete bijection that agrees with every visible card is the
        // authoritative deal. Copy it atomically into the round cache.
        for (const auto& entry : best.entries)
            g_roundCardValues[(size_t)entry.first] = entry.second;
        g_mappingValidated = true;
    } else if (best.directConflicts == 0) {
        // Partial snapshots remain useful, but they may only fill unknown ids;
        // they can never overwrite a value verified from the physical board.
        for (const auto& entry : best.entries) {
            const int id = entry.first;
            const int value = entry.second;
            if (id < 0 || id >= 52 || value < 0 || value >= 52) continue;
            int& cached = g_roundCardValues[(size_t)id];
            if (cached < 0 || cached == value) cached = value;
        }
    }

    // Visible card controllers are a fallback and a live consistency check.
    // The LocalGs mapping above is the only source that also contains cards
    // whose serialized stack value is deliberately hidden as -1.
    void* ray = findRaycastController();
    void* cards = ray ? *(void**)((uint8_t*)ray + 0xA0) : nullptr;
    int count = listSize(cards);
    if (count == 0) {
        void* cardsController = findCardsController();
        cards = cardsController ? *(void**)((uint8_t*)cardsController + 0x60) : nullptr;
        count = listSize(cards);
    }
    std::vector<std::pair<int, int>> controllerValues;
    int controllerMatches = 0;
    int controllerConflicts = 0;
    if (cards && count > 0 && count <= 80) {
        controllerValues.reserve((size_t)count);
        for (int i = 0; i < count; ++i) {
            void* controller = listItem(cards, i);
            if (!controller) continue;
            const int id = *(int*)((uint8_t*)controller + 0x4C);
            const int value = *(int*)((uint8_t*)controller + 0x50);
            if (id < 0 || id >= 52 || value < 0 || value >= 52) continue;
            controllerValues.emplace_back(id, value);
            const auto visible = observed.find(id);
            if (visible != observed.end()) {
                if (visible->second == value) ++controllerMatches;
                else ++controllerConflicts;
            }
        }
    }
    // CardsController is static and can briefly retain the previous deal while
    // a new LocalGs is being attached.  Cache it only after several cards agree
    // with this round's serialized state and none contradict it.
    const int requiredControllerMatches = std::min(4, (int)observed.size());
    if (controllerConflicts == 0 && controllerMatches >= requiredControllerMatches) {
        for (const auto& entry : controllerValues) {
            int& cached = g_roundCardValues[(size_t)entry.first];
            // Controllers may briefly belong to the previous deal. They are a
            // fallback only and must never overwrite an authoritative mapping.
            if (cached < 0 || cached == entry.second) cached = entry.second;
        }
    }

    // A 52-card cache may drive the long perfect-information horizon only when
    // it is a real deck permutation and still agrees with every visible card.
    // If a stale controller ever completes an invalid cache, discard only its
    // unverified values and transparently fall back to the proven V4 behavior.
    bool cacheComplete = true;
    bool cacheValid = true;
    std::array<bool, 52> cacheValues{};
    for (int id = 0; id < 52; ++id) {
        const int value = g_roundCardValues[(size_t)id];
        if (value < 0 || value >= 52) {
            cacheComplete = false;
            continue;
        }
        if (cacheValues[(size_t)value]) cacheValid = false;
        cacheValues[(size_t)value] = true;
        const auto visible = observed.find(id);
        if (visible != observed.end() && visible->second != value)
            cacheValid = false;
    }
    if (cacheComplete && cacheValid) {
        g_mappingValidated = true;
    } else if (cacheComplete) {
        for (int id = 0; id < 52; ++id)
            if (observed.find(id) == observed.end())
                g_roundCardValues[(size_t)id] = -1;
        SOL_LOG("perfect cache rejected reason=non-bijective-or-visible-conflict");
    }

    std::unordered_map<int, int> values;
    values.reserve(52);
    for (int id = 0; id < 52; ++id) {
        const int value = g_roundCardValues[(size_t)id];
        if (value >= 0 && value < 52) values[id] = value;
    }
    if (!g_loggedMappingSnapshot) {
        g_loggedMappingSnapshot = true;
        SOL_LOG("perfect mapping players=%d preferred=%d selected=%d source=%s pairs=%d direct=%d reverse=%d orientation=%s resolved=%d cached=%d validated=%d syncAttempts=%d",
                players, preferredPlayer, best.player,
                best.fromGameStore ? "gamestore" : "localgs",
                (int)best.entries.size(),
                best.directMatches, best.reverseMatches, best.reverse ? "reverse" : "direct",
                (int)observed.size(), (int)values.size(),
                g_mappingValidated ? 1 : 0, g_mappingSyncAttempts);
    }
    if ((int)values.size() > g_lastRoundKnownCount) {
        const uint64_t now = nowMs();
        const int previous = g_lastRoundKnownCount;
        g_lastRoundKnownCount = (int)values.size();
        if (g_lastRoundKnownCount == 52 || now - g_lastPerfectInfoDiagMs >= 1500) {
            g_lastPerfectInfoDiagMs = now;
            SOL_LOG("perfect cache advanced=%d->%d opaque=%d",
                    previous, g_lastRoundKnownCount, 52 - g_lastRoundKnownCount);
        }
    }
    const auto hydrate = [&values](std::vector<Card>& pile) {
        for (Card& card : pile) {
            const auto it = values.find(card.id);
            if (it != values.end()) card.knownValue = it->second;
            else if (card.faceUp()) card.knownValue = card.value;
        }
    };
    for (auto& column : tableau) hydrate(column);
    for (auto& pile : foundations) hydrate(pile);
    hydrate(talon);
    hydrate(waste);
}

static std::string uiStackName(const std::string& stack) {
    // The solo LocalGs owns player-qualified stack keys (p_1_tableau_…).
    // MoveCardTo forwards these strings into the original action payload, so
    // stripping the qualifier makes the native action a no-op.
    return stack;
}

static bool reversesRecentMove(int cardId, const std::string& from,
                               const std::string& to, uint64_t now) {
    // A successful tableau shuffle can otherwise reappear a few seconds
    // later after another card moves. Keep a short tactical memory, not just
    // the immediately preceding edge.
    for (auto it = g_moveHistory.rbegin(); it != g_moveHistory.rend(); ++it) {
        if (now - it->at > 120000) break;
        if (it->cardId == cardId && it->from == to && it->to == from) return true;
    }
    return false;
}

static bool moveCard(void* localGs, const Card& card,
                     const std::string& from, const std::string& to,
                     int targetIndex) {
    if (!card.faceUp() || !g_stringNew || !g_rayMove) return false;
    const uint64_t now = nowMs();
    if (card.id == g_lastMoveId && now - g_lastMoveAttemptMs < 1800 &&
        from == g_lastMoveFrom && to == g_lastMoveTo) {
        // Protect only against re-submitting the same asynchronous drag while
        // its animation is still settling. Strategic loop prevention belongs
        // to the exact GameState history, not to a long-lived card-id ban.
        return false;
    }
    void* ray = findRaycastController();
    void* controller = findCardController(ray, card.id);
    if (!controller) {
        SOL_LOG("move wait card-view id=%d ray=%p", card.id, ray);
        return false;
    }
    // GameRaycastController.cardController (0x58) is the exact card selected
    // by a physical drag. MoveCardTo then creates the game's normal DropCard.
    *(void**)((uint8_t*)ray + 0x58) = controller;
    const std::string target = uiStackName(to);
    const std::string source = uiStackName(from);
    // Match the game controller's own destination index: it is the current
    // number of cards in the destination stack (zero for an empty stack).
    if (targetIndex < 0 || targetIndex > 52) return false;
    g_rayMove(ray, g_stringNew(target.c_str()), targetIndex,
              g_stringNew(source.c_str()), nullptr);
    g_lastActionWasTalon = false;
    g_lastMoveId = card.id;
    g_lastMoveFrom = from;
    g_lastMoveTo = to;
    g_lastMoveAttemptMs = now;
    g_moveHistory.push_back({card.id, from, to, now});
    if (g_moveHistory.size() > 48) g_moveHistory.erase(g_moveHistory.begin(), g_moveHistory.begin() + 16);
    SOL_LOG("ray-move id=%d val=%d from=%s to=%s", card.id, card.value, source.c_str(), target.c_str());
    return true;
}

static std::string tableauName(int index) {
    return g_stackPrefix + "tableau_" + std::to_string(index);
}

static std::string foundationName(int index) {
    return g_stackPrefix + "foundation_" + std::to_string(index);
}

static void rememberSolverState(const GameState& state, uint64_t now) {
    const std::string key = virtualPositionKey(state);
    if (!g_solverStateHistory.empty() && g_solverStateHistory.back().tableau == key) return;
    g_solverStateHistory.push_back({key, now});
    while (g_solverStateHistory.size() > 96 ||
           (!g_solverStateHistory.empty() && now - g_solverStateHistory.front().at > 180000))
        g_solverStateHistory.erase(g_solverStateHistory.begin());
}

static void logPerfectPlannerSnapshot(const GameState& state) {
    if (g_loggedPerfectSnapshot) return;
    g_loggedPerfectSnapshot = true;
    const auto logPile = [](const char* name, const std::vector<Card>& pile) {
        std::string cards;
        cards.reserve(pile.size() * 10);
        for (const Card& card : pile) {
            if (!cards.empty()) cards.push_back(',');
            cards += std::to_string(card.id);
            cards.push_back(':');
            cards += std::to_string(card.known() ?
                                    (card.faceUp() ? card.value : card.knownValue) : -1);
            cards.push_back(card.faceUp() ? 'u' : 'd');
        }
        SOL_LOG("perfect %s=[%s]", name, cards.c_str());
    };
    SOL_LOG("perfect begin draw=%d", state.drawCount);
    for (int i = 0; i < 7; ++i) {
        const std::string name = "tableau_" + std::to_string(i);
        logPile(name.c_str(), state.tableau[i]);
    }
    logPile("talon", state.talon);
    logPile("waste", state.waste);
    for (int i = 0; i < 4; ++i) {
        const std::string name = "foundation_" + std::to_string(i);
        logPile(name.c_str(), state.foundations[i]);
    }
    SOL_LOG("perfect end");
}

static std::string moveKey(const Move& move) {
    return std::to_string((int)move.kind) + ':' + std::to_string(move.from) + ':' +
           std::to_string(move.to) + ':' + std::to_string(move.start);
}

struct PersistentNode {
    GameState state;
    std::vector<Move> line;
    int depth = 0;
    int accumulated = 0;
    int priority = -10000000;
};

struct PersistentNodeLess {
    bool operator()(const PersistentNode& a, const PersistentNode& b) const {
        return a.priority < b.priority;
    }
};

struct RootPlan {
    Move first;
    std::vector<Move> line;
    int score = -10000000;
    int progress = 0;
};

struct PersistentExpertPlanner {
    std::string rootKey;
    std::priority_queue<PersistentNode, std::vector<PersistentNode>, PersistentNodeLess> frontier;
    std::unordered_map<std::string, int> bestDepth;
    std::unordered_map<std::string, RootPlan> roots;
    // Risky foundation-only progress is retained as a last-resort branch, but
    // it never competes with a proven reveal/balancing line.  This avoids both
    // failure modes: racing one suit to King and refusing a necessary legal
    // foundation move forever.
    std::unordered_map<std::string, RootPlan> riskyRoots;
    // Preparatory roots are populated only during strategic recovery.  They
    // preserve the normal solver's conservative behaviour while still giving
    // a stalled board a searched, non-random way to improve mobility, Waste
    // access, or dependency pressure before an irreversible move is possible.
    std::unordered_map<std::string, RootPlan> recoveryRoots;
    // Transposition is intentionally root-edge scoped. Two different first
    // decisions may converge on the same virtual state, but they must retain
    // independent evidence so a failed first branch cannot erase its sibling.
    std::unordered_map<std::string, int> rootHorizon;
    std::unordered_map<std::string, int> rootTerminalLeaves;
    int frames = 0;
    int expanded = 0;
    int maxDepth = 18;
    int rootHidden = 0;
    int rootFoundation = 0;
    int rootFoundationDebt = 0;
    int rootMobility = 0;
    int rootWasteAccess = 0;
    int rootDependencyPenalty = 0;
    int rootStructuralScore = -10000000;
    int rootKnownCards = 0;
    bool endgame = false;
    bool perfectInformation = false;
    bool solved = false;
    bool foundProgress = false;
};

struct ProgressMetrics {
    int foundation = 0;
    int hidden = 0;
    int mobility = 0;
    int productiveMoves = 0;
    int emptyColumns = 0;
    int usefulWasteAccess = 0;
    int stockCycles = 0;
    int repeatedStates = 0;
    int planLength = 0;
    int structuralScore = 0;
    int knownCards = 0;
};

struct DecisionCheckpoint {
    uint64_t id = 0;
    int progressEpoch = 0;
    uint64_t createdAt = 0;
    int moveSerial = 0;
    GameState virtualRoot;
    ProgressMetrics metrics;
    std::string rootExactKey;
    std::string rootSolverKey;
    Move selected;
    std::string selectedMoveKey;
    std::string selectedResultKey;
    std::vector<Move> selectedLine;
    std::vector<std::string> alternatives;
    std::vector<std::string> pathStateKeys;
    int score = -10000000;
    bool failed = false;
};

struct StrategicProgressTracker {
    bool initialized = false;
    std::string exactKey;
    ProgressMetrics baseline;
    ProgressMetrics current;
    int movesWithoutProgress = 0;
    int progressEpoch = 0;
    int moveSerial = 0;
    int recoveryAttemptsAtState = 0;
    uint64_t stateChangedAt = 0;
    bool stallPending = false;
    std::string stallReason;
    std::unordered_map<std::string, int> visitsSinceProgress;
};

static PersistentExpertPlanner g_persistentPlanner;
static std::unordered_set<std::string> g_deadStateCache;
static std::unordered_map<std::string, std::unordered_set<std::string>> g_failedRootMoves;
static std::unordered_map<std::string, std::unordered_set<std::string>> g_failedStrategicBranches;
static std::unordered_map<std::string, int> g_strategicStateFailures;
static std::deque<DecisionCheckpoint> g_decisionCheckpoints;
static StrategicProgressTracker g_progressTracker;
static uint64_t g_nextCheckpointId = 1;
static bool g_recoveryMode = false;
static uint64_t g_recoveryCheckpointId = 0;
static uint64_t g_lastRecoveryAt = 0;
static int g_recoveryGraceTransitions = 0;
// Once every checkpoint and the broader recovery frontier are exhausted for
// one exact physical state, park that state instead of re-entering recovery
// on every UI/raycaster tick.  Any real board change invalidates the latch.
static std::string g_exhaustedRecoveryKey;
static uint64_t g_exhaustedRecoveryAt = 0;
// Recovery depth is remembered per exact physical state.  A state first gets
// the normal expert search, then progressively wider bounded passes; it is
// never allowed to rebuild the same shallow frontier forever.
static std::unordered_map<std::string, int> g_recoveryEscalationByState;
// A relaxed emergency edge may be tried at most once from an exact physical
// state.  This preserves forward motion after the strategic planner has proved
// that no progress-certified root remains, without replacing a freeze with a
// Tableau or Draw-3 ping-pong loop.
static std::unordered_map<std::string, std::unordered_set<std::string>>
    g_emergencyEdgesTried;
static bool g_waitingForMoveResult = false;
static std::string g_pendingRootKey;
static std::string g_pendingExpectedKey;
static std::string g_pendingMoveKey;
static uint64_t g_pendingMoveAt = 0;
static int g_pendingRecycleCountBefore = 0;
static std::vector<Move> g_committedPlan;
static int g_committedPlanScore = -10000000;

static void clearPlannerFrontier() {
    g_persistentPlanner = PersistentExpertPlanner{};
}

static void resetExpertPlanner() {
    clearPlannerFrontier();
    g_deadStateCache.clear();
    g_failedRootMoves.clear();
    g_failedStrategicBranches.clear();
    g_strategicStateFailures.clear();
    g_decisionCheckpoints.clear();
    g_progressTracker = StrategicProgressTracker{};
    g_nextCheckpointId = 1;
    g_recoveryMode = false;
    g_recoveryCheckpointId = 0;
    g_lastRecoveryAt = 0;
    g_recoveryGraceTransitions = 0;
    g_exhaustedRecoveryKey.clear();
    g_exhaustedRecoveryAt = 0;
    g_recoveryEscalationByState.clear();
    g_emergencyEdgesTried.clear();
    g_waitingForMoveResult = false;
    g_pendingRootKey.clear();
    g_pendingExpectedKey.clear();
    g_pendingMoveKey.clear();
    g_pendingMoveAt = 0;
    g_pendingRecycleCountBefore = 0;
    g_committedPlan.clear();
    g_committedPlanScore = -10000000;
}

static int hiddenCardCount(const GameState& state) {
    int hidden = 0;
    for (const auto& column : state.tableau)
        for (const Card& card : column)
            if (!card.revealedForPlanning()) ++hidden;
    return hidden;
}

static bool hasUsefulDestination(const GameState& state, const Card& original) {
    if (!original.known()) return false;
    Card card = original;
    if (!card.faceUp()) card.value = card.knownValue;
    for (int dst = 0; dst < 4; ++dst) {
        const Card* top = state.foundations[dst].empty() ? nullptr :
                          &state.foundations[dst].back();
        if (canDropOnFoundation(card, top)) return true;
    }
    for (int dst = 0; dst < 7; ++dst) {
        const Card* top = state.tableau[dst].empty() ? nullptr :
                          &state.tableau[dst].back();
        if (canDropOnTableau(card, top)) return true;
    }
    return false;
}

static int usefulWasteAccessScore(const GameState& state) {
    int access = 0;
    if (!state.waste.empty() && hasUsefulDestination(state, state.waste.back()))
        access += 4; // immediately executable exposed Waste card

    // Evaluate the exact Draw-N exposure points, not every stock card.  With
    // the native ordering confirmed in OnTalonClicked, the last card of each
    // front draw batch becomes Waste.back().  This metric therefore measures
    // real future accessibility rather than merely noticing a buried card.
    const int draw = std::clamp(state.drawCount, 1, 3);
    for (int offset = 0; offset < (int)state.talon.size(); offset += draw) {
        const int exposed = std::min(offset + draw - 1, (int)state.talon.size() - 1);
        if (hasUsefulDestination(state, state.talon[exposed])) ++access;
    }
    return access;
}

static int productiveMoveCount(const GameState& state) {
    std::vector<Move> moves;
    generateVirtualMoves(state, moves);
    const int hiddenBefore = hiddenCardCount(state);
    const int mobilityBefore = virtualMobility(state);
    int productive = 0;
    for (const Move& move : moves) {
        if (move.kind == VirtualMoveKind::Talon ||
            move.kind == VirtualMoveKind::FoundationToTableau) continue;
        GameState after = state;
        if (!applyVirtualMove(after, move)) continue;
        const bool reveals = hiddenCardCount(after) < hiddenBefore;
        const bool safeFoundation = move.kind == VirtualMoveKind::Foundation &&
                                    foundationRiskDebt(after.foundations) <=
                                    foundationRiskDebt(state.foundations);
        const bool freesWaste = move.from < 0;
        const bool improvesMobility = virtualMobility(after) > mobilityBefore;
        if (reveals || safeFoundation || freesWaste || improvesMobility) ++productive;
    }
    return productive;
}

static ProgressMetrics collectProgressMetrics(const GameState& state, int planLength) {
    ProgressMetrics metrics;
    metrics.foundation = virtualFoundationCount(state);
    metrics.hidden = hiddenCardCount(state);
    metrics.mobility = virtualMobility(state);
    metrics.productiveMoves = productiveMoveCount(state);
    metrics.usefulWasteAccess = usefulWasteAccessScore(state);
    metrics.stockCycles = state.recycles;
    metrics.planLength = planLength;
    metrics.structuralScore = virtualStructuralScore(state);
    for (const auto& column : state.tableau) {
        if (column.empty()) ++metrics.emptyColumns;
        for (const Card& card : column) if (card.known()) ++metrics.knownCards;
    }
    for (const auto& pile : state.foundations)
        for (const Card& card : pile) if (card.known()) ++metrics.knownCards;
    for (const Card& card : state.talon) if (card.known()) ++metrics.knownCards;
    for (const Card& card : state.waste) if (card.known()) ++metrics.knownCards;
    return metrics;
}

static void pruneOldCheckpoints() {
    while (!g_decisionCheckpoints.empty() &&
           (g_decisionCheckpoints.size() > 16 ||
            g_decisionCheckpoints.front().progressEpoch + 1 <
                g_progressTracker.progressEpoch))
        g_decisionCheckpoints.pop_front();
}

static void flagStrategicStall(const char* reason) {
    if (g_progressTracker.stallPending) return;
    g_progressTracker.stallPending = true;
    g_progressTracker.stallReason = reason ? reason : "unknown";
    const ProgressMetrics& base = g_progressTracker.baseline;
    const ProgressMetrics& now = g_progressTracker.current;
    SOL_LOG("[STALL] reason=%s movesWithoutProgress=%d foundationDelta=%d hiddenDelta=%d mobilityDelta=%d productive=%d empty=%d wasteAccess=%d stockCycles=%d repeated=%d planLength=%d known=%d",
            g_progressTracker.stallReason.c_str(),
            g_progressTracker.movesWithoutProgress,
            now.foundation - base.foundation, base.hidden - now.hidden,
            now.mobility - base.mobility, now.productiveMoves,
            now.emptyColumns, now.usefulWasteAccess, now.stockCycles,
            now.repeatedStates, now.planLength, now.knownCards);
}

static void observeStrategicProgress(const GameState& state, uint64_t now) {
    const std::string exactKey = virtualPositionKey(state);
    if (!g_exhaustedRecoveryKey.empty() && g_exhaustedRecoveryKey != exactKey) {
        g_exhaustedRecoveryKey.clear();
        g_exhaustedRecoveryAt = 0;
    }
    if (g_progressTracker.initialized && g_progressTracker.exactKey == exactKey) {
        // The UI and raycaster hooks may both tick several times per frame.
        // Do not regenerate legal moves/evaluation for an unchanged board.
        g_progressTracker.current.planLength = (int)g_committedPlan.size();
        g_progressTracker.current.stockCycles = state.recycles;
        return;
    }
    ProgressMetrics metrics = collectProgressMetrics(state, (int)g_committedPlan.size());
    if (!g_progressTracker.initialized) {
        g_progressTracker.initialized = true;
        g_progressTracker.exactKey = exactKey;
        g_progressTracker.baseline = metrics;
        g_progressTracker.current = metrics;
        g_progressTracker.stateChangedAt = now;
        metrics.repeatedStates = ++g_progressTracker.visitsSinceProgress[solverPositionKey(state)];
        g_progressTracker.current.repeatedStates = metrics.repeatedStates;
        return;
    }
    const ProgressMetrics previous = g_progressTracker.current;
    g_progressTracker.exactKey = exactKey;
    g_progressTracker.stateChangedAt = now;
    g_progressTracker.recoveryAttemptsAtState = 0;
    ++g_progressTracker.moveSerial;
    const bool irreversibleProgress = metrics.foundation > previous.foundation ||
                                      metrics.hidden < previous.hidden;
    if (irreversibleProgress) {
        ++g_progressTracker.progressEpoch;
        g_progressTracker.movesWithoutProgress = 0;
        g_progressTracker.baseline = metrics;
        g_progressTracker.visitsSinceProgress.clear();
        g_progressTracker.stallPending = false;
        g_progressTracker.stallReason.clear();
        g_recoveryMode = false;
        g_recoveryCheckpointId = 0;
        g_recoveryGraceTransitions = 0;
        g_recoveryEscalationByState.clear();
        pruneOldCheckpoints();
    } else {
        ++g_progressTracker.movesWithoutProgress;
        if (g_recoveryGraceTransitions > 0) --g_recoveryGraceTransitions;
    }

    metrics.repeatedStates = ++g_progressTracker.visitsSinceProgress[solverPositionKey(state)];
    g_progressTracker.current = metrics;
    if (irreversibleProgress || g_recoveryGraceTransitions > 0) return;

    const int stockCards = (int)state.talon.size() + (int)state.waste.size();
    const int draw = std::clamp(state.drawCount, 1, 3);
    const int fullPassSteps = std::max(1, (stockCards + draw - 1) / draw + 1);
    int threshold = std::max(10, fullPassSteps + 5);
    if (metrics.hidden <= 7) threshold += 4;
    if (metrics.productiveMoves >= 4) threshold += 2;
    threshold = std::min(threshold, 32);

    const int mobilityDelta = metrics.mobility - g_progressTracker.baseline.mobility;
    const int productiveDelta = metrics.productiveMoves -
                                g_progressTracker.baseline.productiveMoves;
    if (metrics.repeatedStates >= 2 &&
        g_progressTracker.movesWithoutProgress >= std::max(6, fullPassSteps)) {
        flagStrategicStall("repeated-tactical-state");
    } else if (metrics.stockCycles >= 2 && metrics.usefulWasteAccess == 0 &&
               g_progressTracker.movesWithoutProgress >= fullPassSteps * 2) {
        flagStrategicStall("draw3-recycle-without-progress");
    } else if (g_progressTracker.movesWithoutProgress >= threshold &&
               mobilityDelta < 0 && productiveDelta <= 0 &&
               metrics.productiveMoves <= 2) {
        flagStrategicStall("mobility-collapse");
    } else if (g_progressTracker.movesWithoutProgress >= threshold * 2 &&
               (metrics.repeatedStates >= 2 || metrics.stockCycles > 0 ||
                metrics.productiveMoves == 0)) {
        flagStrategicStall("bounded-no-progress");
    }
}

static bool physicalCandidateAllowed(const GameState& state, const Move& move, uint64_t now);
static int immediateMoveTier(const GameState& state, const Move& move);

static void initializePersistentRoot(const GameState& state, const std::string& key) {
    clearPlannerFrontier();
    g_persistentPlanner.rootKey = key;
    g_persistentPlanner.rootHidden = hiddenCardCount(state);
    g_persistentPlanner.rootFoundation = virtualFoundationCount(state);
    g_persistentPlanner.rootFoundationDebt = foundationRiskDebt(state.foundations);
    g_persistentPlanner.rootMobility = virtualMobility(state);
    g_persistentPlanner.rootWasteAccess = usefulWasteAccessScore(state);
    g_persistentPlanner.rootDependencyPenalty = dependencyPenalty(state);
    g_persistentPlanner.rootStructuralScore = virtualStructuralScore(state);
    g_persistentPlanner.rootKnownCards = knownCardCount(state);
    g_persistentPlanner.perfectInformation =
        g_persistentPlanner.rootKnownCards == 52 && !hasOpaqueCards(state);
    g_persistentPlanner.endgame = g_persistentPlanner.rootHidden <= 7;
    if (g_persistentPlanner.perfectInformation) {
        // Perfect information makes long-horizon search deterministic rather
        // than speculative. Depth remains node/time bounded across frames.
        g_persistentPlanner.maxDepth = g_persistentPlanner.endgame ? 144 :
            (g_persistentPlanner.rootHidden <= 14 ? 112 : 80);
    } else {
        // Preserve the proven conservative bounds while IL2CPP mapping is
        // incomplete; an unknown card must never be guessed into a plan.
        g_persistentPlanner.maxDepth = g_persistentPlanner.endgame ? 96 :
            (g_persistentPlanner.rootHidden <= 14 ? 64 : 48);
    }
    const auto escalationIt = g_recoveryEscalationByState.find(key);
    const int escalation = escalationIt == g_recoveryEscalationByState.end() ?
                           0 : escalationIt->second;
    if (g_recoveryMode) {
        const int recoveryBoost = 16 * (1 + escalation);
        g_persistentPlanner.maxDepth = std::min(160,
            g_persistentPlanner.maxDepth + recoveryBoost);
    }
    PersistentNode root;
    root.state = state;
    root.priority = g_persistentPlanner.rootStructuralScore;
    g_persistentPlanner.frontier.push(std::move(root));
    g_persistentPlanner.bestDepth["ROOT|" + solverPositionKey(state)] = 0;
    SOL_LOG("expert planner root endgame=%d recovery=%d depth=%d known=%d/52 perfect=%d escalation=%d deadCache=%d",
            g_persistentPlanner.endgame ? 1 : 0, g_recoveryMode ? 1 : 0,
            g_persistentPlanner.maxDepth, g_persistentPlanner.rootKnownCards,
            g_persistentPlanner.perfectInformation ? 1 : 0, escalation,
            (int)g_deadStateCache.size());
}

static bool rootMoveFailed(const std::string& rootKey, const Move& move) {
    const std::string key = moveKey(move);
    const auto physical = g_failedRootMoves.find(rootKey);
    if (physical != g_failedRootMoves.end() &&
        physical->second.find(key) != physical->second.end()) return true;
    const auto strategic = g_failedStrategicBranches.find(rootKey);
    return strategic != g_failedStrategicBranches.end() &&
           strategic->second.find(key) != strategic->second.end();
}

static int strategicStatePenalty(const std::string& key) {
    const auto it = g_strategicStateFailures.find(key);
    if (it == g_strategicStateFailures.end()) return 0;
    // Strategic failures are evidence, not a mathematical proof.  Penalise a
    // repeatedly bad structure heavily during recovery but never hard-delete
    // it; a later changed Waste/Foundation context may make it necessary.
    const int unit = g_recoveryMode ? 280000 : 65000;
    return std::min(1400000, it->second * unit);
}

static void recordRootPlan(const PersistentNode& node) {
    if (node.line.empty()) return;
    const int hiddenGain = g_persistentPlanner.rootHidden - hiddenCardCount(node.state);
    const int foundationGain = virtualFoundationCount(node.state) -
                               g_persistentPlanner.rootFoundation;
    const int debtAfter = foundationRiskDebt(node.state.foundations);
    const int debtIncrease = std::max(0, debtAfter - g_persistentPlanner.rootFoundationDebt);
    const int debtReduction = std::max(0, g_persistentPlanner.rootFoundationDebt - debtAfter);
    const bool safeFoundationGain = foundationGain > 0 && debtIncrease == 0;
    const bool won = virtualFoundationCount(node.state) == 52;
    // A plan becomes executable only after its branch proves irreversible
    // progress.  Talon-only rotations and cosmetic Tableau shuffles can still
    // be explored as preparation, but can never be selected on their own.
    // Merely dumping a waste card onto Tableau is deliberately not progress:
    // that old shortcut was the main source of mid-game self-congestion.
    // Likewise, a risky foundation push is executable only if this same line
    // also uncovers a hidden card or repays the foundation debt.
    const bool provenProgress = won || hiddenGain > 0 || safeFoundationGain || debtReduction > 0;
    const bool riskyFoundationProgress = !provenProgress && foundationGain > 0;
    const int mobilityGain = virtualMobility(node.state) - g_persistentPlanner.rootMobility;
    const int wasteAccessGain = usefulWasteAccessScore(node.state) -
                                g_persistentPlanner.rootWasteAccess;
    const int dependencyGain = g_persistentPlanner.rootDependencyPenalty -
                               dependencyPenalty(node.state);
    const int structuralGain = virtualStructuralScore(node.state) -
                               g_persistentPlanner.rootStructuralScore;
    const bool recoveryPreparation = g_recoveryMode && !provenProgress &&
                                     !riskyFoundationProgress && debtIncrease == 0 &&
                                     node.depth >= 2 &&
                                     (mobilityGain > 0 || wasteAccessGain > 0 ||
                                      dependencyGain >= 120 || structuralGain >= 900);
    if (!provenProgress && !riskyFoundationProgress && !recoveryPreparation) return;
    int progress = (won ? 2000000 : 0) + hiddenGain * 100000 +
                   foundationGain * 4200 + debtReduction * 12000 -
                   debtIncrease * 18000 - node.depth * 24;
    if (recoveryPreparation)
        progress += mobilityGain * 5200 + wasteAccessGain * 2800 +
                    dependencyGain * 22 + std::max(0, structuralGain) / 3;
    const int planScore = node.priority + progress;
    const std::string firstKey = moveKey(node.line.front());
    auto& destination = provenProgress ? g_persistentPlanner.roots :
                        (riskyFoundationProgress ? g_persistentPlanner.riskyRoots :
                                                   g_persistentPlanner.recoveryRoots);
    auto it = destination.find(firstKey);
    if (it == destination.end() || planScore > it->second.score)
        destination[firstKey] = {node.line.front(), node.line, planScore, progress};
    if (provenProgress) g_persistentPlanner.foundProgress = true;
}

static void advancePersistentSearch(const GameState& rootState) {
    ++g_persistentPlanner.frames;
    const bool endgame = g_persistentPlanner.endgame;
    // Batch the same search work into fewer Unity frames. The expanded-node
    // proof thresholds remain unchanged, so Turbo improves response latency
    // without lowering the solver's tactical evidence requirement. The hard
    // millisecond deadline keeps weak devices inside a frame-safe budget.
    const int baseNodes = g_searchSliceNodes.load(std::memory_order_relaxed);
    const int baseSliceMs = g_searchSliceMs.load(std::memory_order_relaxed);
    // Normal play stays within the loader's frame budget. Endgame and recovery
    // receive bounded multipliers because a wider proof here is cheaper than a
    // wrong physical move. The millisecond deadline always remains authoritative.
    const int frameNodeBudget = g_recoveryMode
        ? (endgame ? baseNodes * 2 : baseNodes + baseNodes / 2)
        : (endgame ? baseNodes + baseNodes / 2 : baseNodes);
    const int frameSliceMs = baseSliceMs + (endgame ? 2 : 0) +
                             (g_recoveryMode ? 2 : 0);
    const uint64_t deadline = nowMs() + (uint64_t)frameSliceMs;
    int expandedThisFrame = 0;

    while (!g_persistentPlanner.frontier.empty() &&
           expandedThisFrame < frameNodeBudget && nowMs() < deadline) {
        PersistentNode node = g_persistentPlanner.frontier.top();
        g_persistentPlanner.frontier.pop();
        if (!node.line.empty() &&
            rootMoveFailed(g_persistentPlanner.rootKey, node.line.front())) continue;
        ++expandedThisFrame;
        ++g_persistentPlanner.expanded;
        if (virtualFoundationCount(node.state) == 52) {
            node.priority += 2000000;
            recordRootPlan(node);
            g_persistentPlanner.solved = true;
            break;
        }
        if (node.depth >= g_persistentPlanner.maxDepth) continue;

        std::vector<Move> moves;
        generateVirtualMoves(node.state, moves,
            g_recoveryMode && g_persistentPlanner.perfectInformation);
        bool generated = false;
        for (const Move& move : moves) {
            if (node.line.empty()) {
                if (rootMoveFailed(g_persistentPlanner.rootKey, move)) continue;
                if (!physicalCandidateAllowed(rootState, move, nowMs())) continue;
            }
            GameState next = node.state;
            if (!applyVirtualMove(next, move)) continue;
            const std::string nextExactKey = virtualPositionKey(next);
            const std::string nextKey = solverPositionKey(next);
            if (g_deadStateCache.find(nextKey) != g_deadStateCache.end()) continue;
            const int nextDepth = node.depth + 1;
            const std::string firstEdge = node.line.empty() ? moveKey(move) :
                                          moveKey(node.line.front());
            const std::string visitKey = firstEdge + '|' + nextKey;
            const auto visited = g_persistentPlanner.bestDepth.find(visitKey);
            if (visited != g_persistentPlanner.bestDepth.end() && visited->second <= nextDepth) continue;
            g_persistentPlanner.bestDepth[visitKey] = nextDepth;
            int& horizon = g_persistentPlanner.rootHorizon[firstEdge];
            horizon = std::max(horizon, nextDepth);

            PersistentNode child;
            child.state = std::move(next);
            child.line = node.line;
            child.line.push_back(move);
            child.depth = nextDepth;
            child.accumulated = node.accumulated +
                                candidateTacticalBonus(node.state, child.state, move);
            child.priority = virtualStructuralScore(child.state) + child.accumulated -
                             nextDepth * 18;
            if (seenRecentSolverState(nextExactKey, nowMs())) child.priority -= 4000000;
            child.priority -= strategicStatePenalty(nextKey);
            recordRootPlan(child);
            g_persistentPlanner.frontier.push(std::move(child));
            generated = true;
        }
        // Cache only rule-terminal states globally. A node whose children were
        // merely transposed is already represented elsewhere in this search;
        // calling it dead would incorrectly poison that equivalent live path.
        if (!generated && moves.empty() && virtualFoundationCount(node.state) < 52 &&
            !hasOpaqueCards(node.state)) {
            g_deadStateCache.insert(solverPositionKey(node.state));
            if (!node.line.empty()) {
                int& terminal = g_persistentPlanner.rootTerminalLeaves[
                    moveKey(node.line.front())];
                terminal = std::min(64, terminal + 1);
            }
        }
    }
}

static int requiredRootHorizon(const PersistentExpertPlanner& planner) {
    if (!planner.perfectInformation) return 5;
    if (planner.endgame) return 24;
    if (planner.rootHidden <= 14) return 16;
    return 12;
}

static int rootHorizonFor(const Move& move) {
    const auto it = g_persistentPlanner.rootHorizon.find(moveKey(move));
    return it == g_persistentPlanner.rootHorizon.end() ? 0 : it->second;
}

static int rootTerminalLeavesFor(const Move& move) {
    const auto it = g_persistentPlanner.rootTerminalLeaves.find(moveKey(move));
    return it == g_persistentPlanner.rootTerminalLeaves.end() ? 0 : it->second;
}

static int bestExecutableRootHorizon(const std::string& rootKey) {
    int best = 0;
    const auto consider = [&](const auto& plans) {
        for (const auto& entry : plans) {
            if (rootMoveFailed(rootKey, entry.second.first)) continue;
            best = std::max(best, rootHorizonFor(entry.second.first));
        }
    };
    consider(g_persistentPlanner.roots);
    if (g_recoveryMode) consider(g_persistentPlanner.recoveryRoots);
    return best;
}

static void discardPlannerRootEdge(const std::string& rootKey,
                                   const std::string& failedMoveKey) {
    if (g_persistentPlanner.rootKey != rootKey) return;
    g_persistentPlanner.roots.erase(failedMoveKey);
    g_persistentPlanner.riskyRoots.erase(failedMoveKey);
    g_persistentPlanner.recoveryRoots.erase(failedMoveKey);
    // Frontier nodes are intentionally retained. advancePersistentSearch()
    // lazily drops only nodes whose first edge is now failed, preserving every
    // still-valid sibling and all accumulated transposition work.
}

static void registerPhysicalRootFailure(const std::string& rootKey,
                                        const std::string& failedMoveKey) {
    g_failedRootMoves[rootKey].insert(failedMoveKey);
    discardPlannerRootEdge(rootKey, failedMoveKey);
}

static std::vector<std::string> legalAlternativeKeys(const GameState& state,
                                                     const Move& selected) {
    std::vector<Move> legal;
    const bool perfectRecovery = g_recoveryMode &&
        knownCardCount(state) == 52 && !hasOpaqueCards(state);
    generateVirtualMoves(state, legal, perfectRecovery);
    std::vector<std::string> alternatives;
    const std::string selectedKey = moveKey(selected);
    const std::string rootKey = virtualPositionKey(state);
    alternatives.reserve(legal.size());
    for (const Move& move : legal) {
        const std::string key = moveKey(move);
        if (key != selectedKey && !rootMoveFailed(rootKey, move))
            alternatives.push_back(key);
    }
    std::sort(alternatives.begin(), alternatives.end());
    alternatives.erase(std::unique(alternatives.begin(), alternatives.end()),
                       alternatives.end());
    return alternatives;
}

static void saveDecisionCheckpoint(const GameState& root,
                                   const Move& selected,
                                   const std::vector<Move>& selectedLine,
                                   std::vector<std::string> alternatives,
                                   int score) {
    // A single forced move has no branch to backtrack.  A multi-step committed
    // route is still important because its later virtual states can be used as
    // strategic-failure evidence even if the root had no immediate sibling.
    if (alternatives.empty() && selectedLine.size() <= 1) return;
    const std::string rootExact = virtualPositionKey(root);
    const std::string selectedKey = moveKey(selected);
    if (!g_decisionCheckpoints.empty()) {
        const DecisionCheckpoint& last = g_decisionCheckpoints.back();
        if (!last.failed && last.rootExactKey == rootExact &&
            last.selectedMoveKey == selectedKey) return;
    }

    DecisionCheckpoint checkpoint;
    checkpoint.id = g_nextCheckpointId++;
    checkpoint.progressEpoch = g_progressTracker.progressEpoch;
    checkpoint.createdAt = nowMs();
    checkpoint.moveSerial = g_progressTracker.moveSerial;
    checkpoint.virtualRoot = root;
    checkpoint.metrics = g_progressTracker.initialized &&
                         g_progressTracker.exactKey == rootExact ?
                         g_progressTracker.current :
                         collectProgressMetrics(root, (int)selectedLine.size());
    checkpoint.rootExactKey = rootExact;
    checkpoint.rootSolverKey = solverPositionKey(root);
    checkpoint.selected = selected;
    checkpoint.selectedMoveKey = selectedKey;
    checkpoint.selectedLine = selectedLine;
    checkpoint.alternatives = std::move(alternatives);
    checkpoint.score = score;

    GameState cursor = root;
    const int pathLimit = std::min(24, (int)selectedLine.size());
    for (int i = 0; i < pathLimit; ++i) {
        if (!applyVirtualMove(cursor, selectedLine[i])) break;
        const std::string stateKey = solverPositionKey(cursor);
        checkpoint.pathStateKeys.push_back(stateKey);
        if (i == 0) checkpoint.selectedResultKey = stateKey;
    }
    if (checkpoint.selectedResultKey.empty()) {
        GameState first = root;
        if (applyVirtualMove(first, selected))
            checkpoint.selectedResultKey = solverPositionKey(first);
    }

    g_decisionCheckpoints.push_back(std::move(checkpoint));
    pruneOldCheckpoints();
}

static DecisionCheckpoint* latestRecoveryCheckpoint() {
    // Prefer the current no-progress epoch.  If every move since the last
    // irreversible gain was forced, allow the immediately preceding epoch's
    // latest decision: a superficially useful foundation/reveal move can still
    // be the strategic choice that closed the winning continuation.
    for (int pass = 0; pass < 2; ++pass) {
        const int wantedEpoch = g_progressTracker.progressEpoch - pass;
        if (wantedEpoch < 0) continue;
        for (auto it = g_decisionCheckpoints.rbegin();
             it != g_decisionCheckpoints.rend(); ++it) {
            if (it->failed || it->progressEpoch != wantedEpoch) continue;
            if (g_progressTracker.moveSerial - it->moveSerial > 28) continue;
            return &*it;
        }
    }
    return nullptr;
}

static int remainingCheckpointAlternatives(const DecisionCheckpoint& checkpoint) {
    int remaining = 0;
    const auto failed = g_failedStrategicBranches.find(checkpoint.rootExactKey);
    for (const std::string& key : checkpoint.alternatives) {
        if (failed == g_failedStrategicBranches.end() ||
            failed->second.find(key) == failed->second.end()) ++remaining;
    }
    return remaining;
}

static void failStrategicCheckpoint(DecisionCheckpoint& checkpoint) {
    checkpoint.failed = true;
    g_failedStrategicBranches[checkpoint.rootExactKey].insert(
        checkpoint.selectedMoveKey);
    discardPlannerRootEdge(checkpoint.rootExactKey,
                           checkpoint.selectedMoveKey);
    for (const std::string& key : checkpoint.pathStateKeys) {
        int& failures = g_strategicStateFailures[key];
        failures = std::min(5, failures + 1);
    }
    if (!checkpoint.selectedResultKey.empty()) {
        int& failures = g_strategicStateFailures[checkpoint.selectedResultKey];
        failures = std::min(5, failures + 1);
    }
}

static int strategicLinePenalty(const GameState& root,
                                const std::vector<Move>& line) {
    GameState cursor = root;
    int penalty = 0;
    const int limit = std::min(20, (int)line.size());
    for (int i = 0; i < limit; ++i) {
        if (!applyVirtualMove(cursor, line[i])) return 1500000;
        if (virtualFoundationCount(cursor) == 52) return 0;
        const int statePenalty = strategicStatePenalty(solverPositionKey(cursor));
        // Earlier re-entry into a failed structure is stronger evidence than
        // a distant overlap late in an otherwise distinct line.
        penalty += statePenalty / (1 + i / 4);
        if (penalty >= 1800000) return 1800000;
    }
    return penalty;
}

static bool moveCreatesIrreversibleProgress(const GameState& state,
                                            const Move& move) {
    GameState after = state;
    if (!applyVirtualMove(after, move)) return false;
    return hiddenCardCount(after) < hiddenCardCount(state) ||
           virtualFoundationCount(after) > virtualFoundationCount(state) ||
           foundationRiskDebt(after.foundations) < foundationRiskDebt(state.foundations);
}

// A plan may contain several preparatory moves before it reaches the reveal or
// safe Foundation advance that made the branch attractive.  The old executor
// committed the complete searched line, including everything *after* that
// milestone.  That let a good search result drift into a later cosmetic
// Tableau shuffle without asking the solver to evaluate the freshly observed
// board again.  A certified prefix ends exactly at the first real milestone.
struct CertifiedPlanPrefix {
    bool valid = false;
    int progressIndex = -1;
    int failureIndex = -1;
    std::string reason;
    std::string failureStateKey;
};

static bool canonicalStateVisitedSinceProgress(const std::string& key) {
    if (!g_progressTracker.initialized) return false;
    const auto it = g_progressTracker.visitsSinceProgress.find(key);
    return it != g_progressTracker.visitsSinceProgress.end() && it->second > 0;
}

static CertifiedPlanPrefix certifyPlanPrefix(const GameState& root,
                                              const std::vector<Move>& line,
                                              bool allowRiskyFoundation,
                                              bool consultPhysicalHistory = true,
                                              bool allowRecoveryPreparation = false) {
    CertifiedPlanPrefix result;
    if (line.empty()) {
        result.reason = "empty-plan";
        return result;
    }

    const int rootHidden = hiddenCardCount(root);
    const int rootFoundation = virtualFoundationCount(root);
    const int rootDebt = foundationRiskDebt(root.foundations);
    const int rootMobility = virtualMobility(root);
    const int rootWasteAccess = usefulWasteAccessScore(root);
    const int rootDependency = dependencyPenalty(root);
    const int rootStructural = virtualStructuralScore(root);
    GameState cursor = root;
    std::unordered_set<std::string> localCanonicalStates;
    localCanonicalStates.insert(solverPositionKey(root));

    for (int i = 0; i < (int)line.size(); ++i) {
        if (!applyVirtualMove(cursor, line[i])) {
            result.failureIndex = i;
            result.reason = "illegal-virtual-step";
            return result;
        }

        const int hiddenNow = hiddenCardCount(cursor);
        const int foundationNow = virtualFoundationCount(cursor);
        const int debtNow = foundationRiskDebt(cursor.foundations);
        const bool won = foundationNow == 52;
        const bool hiddenGain = hiddenNow < rootHidden;
        const bool safeFoundationGain = foundationNow > rootFoundation &&
                                        debtNow <= rootDebt;
        const bool debtReduction = debtNow < rootDebt;
        const bool acceptedRiskyGain = allowRiskyFoundation &&
                                        foundationNow > rootFoundation;
        // recordRootPlan() deliberately retains a searched bridge move during
        // strategic recovery when it improves mobility, Waste accessibility,
        // dependency pressure, or the global structure without adding
        // Foundation debt. The executor previously had no matching definition,
        // so every one of those retained roots was later rejected as
        // "no-proven-progress" and the live board appeared frozen. Accept the
        // first matching bridge only in Recovery mode; normal play still
        // requires an irreversible reveal/Foundation milestone.
        const bool recoveryPreparation = allowRecoveryPreparation && i >= 1 &&
            debtNow <= rootDebt && foundationNow <= rootFoundation &&
            (virtualMobility(cursor) > rootMobility ||
             usefulWasteAccessScore(cursor) > rootWasteAccess ||
             rootDependency - dependencyPenalty(cursor) >= 120 ||
             virtualStructuralScore(cursor) - rootStructural >= 900);
        const bool reachedMilestone = won || hiddenGain || safeFoundationGain ||
                                      debtReduction || acceptedRiskyGain ||
                                      recoveryPreparation;
        const std::string canonicalKey = solverPositionKey(cursor);

        // Column indices are presentation details. Re-entering the same
        // canonical structure before making progress is the exact signature
        // of King/sequence ping-pong, even when the physical column numbers
        // differ and virtualPositionKey therefore looks new.
        if (!reachedMilestone &&
            localCanonicalStates.find(canonicalKey) != localCanonicalStates.end()) {
            result.failureIndex = i;
            result.reason = "canonical-cycle-in-plan";
            result.failureStateKey = canonicalKey;
            return result;
        }
        if (!reachedMilestone && consultPhysicalHistory &&
            canonicalStateVisitedSinceProgress(canonicalKey)) {
            result.failureIndex = i;
            result.reason = "visited-no-progress-state";
            result.failureStateKey = canonicalKey;
            return result;
        }
        localCanonicalStates.insert(canonicalKey);

        if (reachedMilestone) {
            result.valid = true;
            result.progressIndex = i;
            result.reason = won ? "win" :
                            (hiddenGain ? "hidden-reveal" :
                             (debtReduction ? "foundation-debt-reduced" :
                              (safeFoundationGain ? "safe-foundation" :
                               (acceptedRiskyGain ? "risk-bounded-foundation" :
                                                    "recovery-preparation"))));
            return result;
        }
    }

    result.failureIndex = (int)line.size();
    result.reason = "no-proven-progress";
    result.failureStateKey = solverPositionKey(cursor);
    return result;
}

static bool runPlanGuardSelfTests() {
    Card king;
    king.id = 11;
    king.value = 11; // Jawaker value 11 maps to King (rank 12).
    king.knownValue = 11;
    GameState left;
    left.tableau[0].push_back(king);
    GameState right = left;
    right.tableau[0].clear();
    right.tableau[5].push_back(king);
    const bool canonicalColumns = solverPositionKey(left) == solverPositionKey(right) &&
                                  virtualPositionKey(left) != virtualPositionKey(right);

    // A full Draw-3 followed by an immediate recycle returns to the same
    // structural state and must be rejected as a no-progress cycle.
    GameState recycle;
    recycle.drawCount = 3;
    for (int i = 0; i < 3; ++i) {
        Card card;
        card.id = 20 + i;
        card.value = i;
        card.knownValue = i;
        recycle.talon.push_back(card);
    }
    const std::vector<Move> recycleLine{
        {VirtualMoveKind::Talon, -1, -1, -1},
        {VirtualMoveKind::Talon, -1, -1, -1}
    };
    const CertifiedPlanPrefix cycle = certifyPlanPrefix(
        recycle, recycleLine, false, false);
    const bool rejectsCycle = !cycle.valid &&
                              cycle.reason == "canonical-cycle-in-plan";

    // Moving the exposed sequence below reveals a known face-down card.  This
    // is genuine progress and the certified prefix must end at that first move.
    GameState reveal;
    Card hidden;
    hidden.id = 31;
    hidden.value = -1;
    hidden.knownValue = 0;
    Card moving;
    moving.id = 32;
    moving.value = 4;  // rank 5, black
    moving.knownValue = 4;
    Card destination;
    destination.id = 33;
    destination.value = 18; // rank 6, red
    destination.knownValue = 18;
    reveal.tableau[0] = {hidden, moving};
    reveal.tableau[1] = {destination};
    const std::vector<Move> revealLine{
        {VirtualMoveKind::Tableau, 0, 1, 1}
    };
    const CertifiedPlanPrefix progress = certifyPlanPrefix(
        reveal, revealLine, false, false);
    const bool acceptsReveal = progress.valid && progress.progressIndex == 0;

    SOL_LOG("[PLAN_GUARD_TEST] canonical=%d cycle=%d reveal=%d",
            canonicalColumns ? 1 : 0, rejectsCycle ? 1 : 0,
            acceptsReveal ? 1 : 0);
    return canonicalColumns && rejectsCycle && acceptsReveal;
}

static bool activateStrategicRecovery(const GameState& physicalState,
                                      const std::string& physicalKey) {
    DecisionCheckpoint* checkpoint = latestRecoveryCheckpoint();
    // If logical checkpoints are exhausted, progressively widen a bounded
    // perfect-information search from the CURRENT physical state. Never replay
    // an old board physically and never rebuild the same shallow frontier.
    if (!checkpoint && g_progressTracker.recoveryAttemptsAtState > 0) {
        int& escalation = g_recoveryEscalationByState[physicalKey];
        if (escalation < SOL_MAX_RECOVERY_ESCALATION) {
            ++escalation;
            ++g_progressTracker.recoveryAttemptsAtState;
            g_recoveryMode = true;
            g_recoveryCheckpointId = 0;
            g_lastRecoveryAt = nowMs();
            g_recoveryGraceTransitions = 0;
            g_committedPlan.clear();
            g_exhaustedRecoveryKey.clear();
            g_exhaustedRecoveryAt = 0;
            g_progressTracker.stallPending = false;
            g_progressTracker.stallReason.clear();
            initializePersistentRoot(physicalState, physicalKey);
            SOL_LOG("[RECOVERY] checkpoint=0 escalation=%d/%d known=%d/52 depth=%d",
                    escalation, SOL_MAX_RECOVERY_ESCALATION,
                    knownCardCount(physicalState), g_persistentPlanner.maxDepth);
            return true;
        }
        g_recoveryMode = false;
        g_recoveryCheckpointId = 0;
        g_recoveryGraceTransitions = 0;
        g_committedPlan.clear();
        g_exhaustedRecoveryKey = physicalKey;
        g_exhaustedRecoveryAt = nowMs();
        clearPlannerFrontier();
        g_progressTracker.stallPending = false;
        g_progressTracker.stallReason.clear();
        SOL_LOG("[RECOVERY_RESULT] exhausted checkpoint=0 opaque=%d known=%d escalation=%d; cooldown=%llums",
                hasOpaqueCards(physicalState) ? 1 : 0,
                knownCardCount(physicalState), escalation,
                (unsigned long long)SOL_EXHAUSTED_RETRY_MS);
        return false;
    }

    uint64_t checkpointId = 0;
    std::string failedBranch = "none";
    int alternativesRemaining = 0;
    if (checkpoint) {
        checkpointId = checkpoint->id;
        failedBranch = checkpoint->selectedMoveKey;
        failStrategicCheckpoint(*checkpoint);
        alternativesRemaining = remainingCheckpointAlternatives(*checkpoint);
    }

    ++g_progressTracker.recoveryAttemptsAtState;
    g_recoveryMode = true;
    g_recoveryCheckpointId = checkpointId;
    g_lastRecoveryAt = nowMs();
    g_recoveryGraceTransitions = 0;
    g_committedPlan.clear();
    g_progressTracker.stallPending = false;
    SOL_LOG("[RECOVERY] checkpoint=%llu failedBranch=%s alternativesRemaining=%d",
            (unsigned long long)checkpointId, failedBranch.c_str(),
            alternativesRemaining);

    // Preserve a live current-root frontier and its sibling alternatives.
    // Reinitialise only when it belongs to another physical state or has been
    // fully exhausted; this is deliberately later than plan/alternative
    // recovery, never the first action.
    const bool currentPlannerUsable = g_persistentPlanner.rootKey == physicalKey &&
        (!g_persistentPlanner.frontier.empty() || !g_persistentPlanner.roots.empty() ||
         !g_persistentPlanner.riskyRoots.empty() ||
         !g_persistentPlanner.recoveryRoots.empty());
    if (currentPlannerUsable) {
        g_persistentPlanner.maxDepth = std::min(160,
            std::max(g_persistentPlanner.maxDepth,
                     g_persistentPlanner.endgame ? 144 : 96));
    } else {
        initializePersistentRoot(physicalState, physicalKey);
    }
    return true;
}

static void detectPlannerIdleStall(const std::string& key, uint64_t now) {
    if (!g_progressTracker.initialized || g_progressTracker.stallPending ||
        g_waitingForMoveResult || !g_committedPlan.empty()) return;
    if (g_progressTracker.exactKey != key ||
        g_persistentPlanner.rootKey != key) return;
    const uint64_t idleStart = std::max(g_progressTracker.stateChangedAt,
                                        g_lastRecoveryAt);
    const uint64_t idleMs = now > idleStart ? now - idleStart : 0;
    const bool noPlan = g_persistentPlanner.roots.empty() &&
                        g_persistentPlanner.riskyRoots.empty() &&
                        g_persistentPlanner.recoveryRoots.empty();
    if (!noPlan) return;
    if (g_persistentPlanner.frontier.empty() && idleMs >= 1800) {
        flagStrategicStall("planner-frontier-exhausted");
    } else if (g_persistentPlanner.frames >= 8 &&
               idleMs >= (g_persistentPlanner.endgame ? 9000ULL : 6500ULL)) {
        flagStrategicStall("planner-no-executable-continuation");
    }
}

static bool physicalCandidateAllowed(const GameState& state, const Move& move, uint64_t now) {
    GameState after = state;
    if (!applyVirtualMove(after, move)) return false;

    // Keep the old exact-state guard for physical Tableau reversals. It is
    // intentionally not applied to a legal Foundation pullback because the
    // Foundation/Waste context may have changed and made that rescue useful.
    if (move.kind == VirtualMoveKind::Tableau && move.from >= 0 &&
        seenRecentSolverState(virtualPositionKey(after), now)) {
        return false;
    }

    // The missing half of the old guard: columns are symmetric in Solitaire.
    // During one no-progress epoch, never execute a move that returns to a
    // canonical structure already reached under different column indices.
    // A reveal/Foundation advance starts a new epoch and remains admissible.
    if (!moveCreatesIrreversibleProgress(state, move) &&
        canonicalStateVisitedSinceProgress(solverPositionKey(after))) {
        return false;
    }
    return true;
}

static bool executeExpertMove(void* localGs, const GameState& state,
                              const Move& move, int score) {
    const uint64_t now = nowMs();
    if (move.kind == VirtualMoveKind::Talon) {
        if (!g_talonUi || (state.talon.empty() && state.waste.empty())) return false;
        g_talonUi(nullptr);
        g_lastActionWasTalon = true;
        if (state.talon.empty()) ++g_actualRecycleCount;
        g_lastTalonMs = now;
        g_lastMoveMs = now;
        SOL_LOG("expert talon mode=%s score=%d stock=%d waste=%d",
                state.talon.empty() ? "recycle" : "draw", score,
                (int)state.talon.size(), (int)state.waste.size());
        return true;
    }
    if (move.kind == VirtualMoveKind::Foundation) {
        if (move.to < 0 || move.to >= 4) return false;
        const Card* card = nullptr;
        std::string source;
        if (move.from < 0) {
            if (state.waste.empty()) return false;
            card = &state.waste.back();
            source = g_stackPrefix + "waste";
        } else {
            if (move.from >= 7 || state.tableau[move.from].empty()) return false;
            card = &state.tableau[move.from].back();
            source = tableauName(move.from);
        }
        if (!card->faceUp()) return false;
        const bool moved = moveCard(localGs, *card, source, foundationName(move.to),
                                    (int)state.foundations[move.to].size());
        if (moved) {
            g_actualRecycleCount = 0;
            SOL_LOG("expert foundation id=%d from=%d to=%d score=%d",
                    card->id, move.from, move.to, score);
        }
        return moved;
    }
    if (move.kind == VirtualMoveKind::FoundationToTableau) {
        if (move.from < 0 || move.from >= 4 || move.to < 0 || move.to >= 7 ||
            state.foundations[move.from].empty()) return false;
        const Card& card = state.foundations[move.from].back();
        const bool moved = moveCard(localGs, card, foundationName(move.from), tableauName(move.to),
                                    (int)state.tableau[move.to].size());
        if (moved) SOL_LOG("expert foundation-rescue id=%d from=%d to=%d score=%d",
                           card.id, move.from, move.to, score);
        return moved;
    }
    if (move.to < 0 || move.to >= 7) return false;
    const Card* card = nullptr;
    std::string source;
    if (move.from < 0) {
        if (state.waste.empty()) return false;
        card = &state.waste.back();
        source = g_stackPrefix + "waste";
    } else {
        if (move.from >= 7 || move.start < 0 ||
            move.start >= (int)state.tableau[move.from].size()) return false;
        card = &state.tableau[move.from][move.start];
        source = tableauName(move.from);
    }
    if (!card->faceUp()) return false;
    const bool moved = moveCard(localGs, *card, source, tableauName(move.to),
                                (int)state.tableau[move.to].size());
    if (moved) {
        g_actualRecycleCount = 0;
        g_rrSource = move.from >= 0 ? (move.from + 1) % 8 : 0;
        SOL_LOG("expert tableau id=%d from=%d to=%d start=%d score=%d",
                card->id, move.from, move.to, move.start, score);
    }
    return moved;
}

// Emergency moves used to bypass the normal pending-action handshake.  A UI
// call could therefore report success while the physical board stayed exactly
// the same, after which the bot clicked the same Talon/card every 650 ms.  Route
// every escape move through the same expected-state validation as a normal
// planner move and remember the edge immediately, so a failed/no-op action can
// never become an infinite visual loop.
static bool executeTrackedEmergencyMove(void* localGs, const GameState& state,
                                        const Move& move, int score,
                                        const char* reason) {
    const std::string rootKey = virtualPositionKey(state);
    const std::string edgeKey = moveKey(move);
    auto& tried = g_emergencyEdgesTried[rootKey];
    if (tried.find(edgeKey) != tried.end() || rootMoveFailed(rootKey, move))
        return false;
    if (!physicalCandidateAllowed(state, move, nowMs())) {
        tried.insert(edgeKey);
        return false;
    }

    GameState expected = state;
    if (!applyVirtualMove(expected, move)) {
        tried.insert(edgeKey);
        return false;
    }

    const int recycleCountBefore = g_actualRecycleCount;
    // Mark the edge even when the immediate native submission fails.  Retrying
    // the identical ray/UI call from an unchanged physical state cannot help;
    // the next legal alternative is the correct recovery action.
    tried.insert(edgeKey);
    if (!executeExpertMove(localGs, state, move, score)) {
        registerPhysicalRootFailure(rootKey, edgeKey);
        SOL_LOG("emergency submit-failed reason=%s move=%s",
                reason ? reason : "unknown", edgeKey.c_str());
        return false;
    }

    g_pendingRootKey = rootKey;
    g_pendingExpectedKey = virtualPositionKey(expected);
    g_pendingMoveKey = edgeKey;
    g_pendingMoveAt = nowMs();
    g_pendingRecycleCountBefore = recycleCountBefore;
    g_waitingForMoveResult = true;
    SOL_LOG("emergency selected reason=%s move=%s score=%d expected=%s",
            reason ? reason : "unknown", edgeKey.c_str(), score,
            g_pendingExpectedKey.c_str());
    return true;
}

static bool attemptEmergencyProgress(void* localGs, const GameState& state) {
    // This path is entered only after the retained planner has no executable
    // root (or after several no-progress stock passes).  It deliberately
    // performs only locally proven legal moves; its purpose is to prevent a
    // live board from becoming visually frozen because a search branch was
    // exhausted or over-penalised.

    // First free a hidden Tableau card by advancing its exposed blocker to a
    // matching foundation.  This is stronger than another blind stock pass,
    // even when the normal conservative foundation threshold would defer it.
    for (int src = 0; src < 7; ++src) {
        if (state.tableau[src].empty()) continue;
        const Card& card = state.tableau[src].back();
        if (!card.faceUp()) continue;
        const bool revealsHidden = state.tableau[src].size() > 1 &&
                                   !state.tableau[src][state.tableau[src].size() - 2].faceUp();
        if (!revealsHidden && !foundationAdvanceIsSafe(card, state.foundations)) continue;
        for (int dst = 0; dst < 4; ++dst) {
            const Card* top = state.foundations[dst].empty() ? nullptr : &state.foundations[dst].back();
            if (!canDropOnFoundation(card, top)) continue;
            const Move move{VirtualMoveKind::Foundation, src, dst, -1};
            if (executeTrackedEmergencyMove(localGs, state, move,
                                            revealsHidden ? 180000 : 120000,
                                            revealsHidden ? "foundation-reveal" :
                                                            "safe-foundation")) {
                return true;
            }
        }
    }

    // A playable Waste card changes the Draw-3 alignment.  Never leave it on
    // screen while repeatedly recycling the same deck.
    if (!state.waste.empty() && state.waste.back().faceUp()) {
        const Card& card = state.waste.back();
        for (int dst = 0; dst < 4; ++dst) {
            const Card* top = state.foundations[dst].empty() ? nullptr : &state.foundations[dst].back();
            if (!canDropOnFoundation(card, top)) continue;
            const Move move{VirtualMoveKind::Foundation, -1, dst, -1};
            if (executeTrackedEmergencyMove(localGs, state, move, 100000,
                                            "waste-foundation")) {
                return true;
            }
        }
        for (int dst = 0; dst < 7; ++dst) {
            const Card* top = state.tableau[dst].empty() ? nullptr : &state.tableau[dst].back();
            if (!canDropOnTableau(card, top)) continue;
            const Move move{VirtualMoveKind::Tableau, -1, dst, -1};
            if (executeTrackedEmergencyMove(localGs, state, move, 90000,
                                            "waste-tableau")) {
                return true;
            }
        }
    }

    // Finally accept a legal sequence move only when it immediately exposes a
    // hidden card.  Cosmetic shuffles and King-to-empty reversals remain
    // forbidden by isStrategicTableauMove and the recent-move memory.
    for (int src = 0; src < 7; ++src) {
        const int first = firstFaceUpIndex(state.tableau[src]);
        if (first <= 0 || state.tableau[src][first - 1].faceUp()) continue;
        const Card& card = state.tableau[src][first];
        for (int dst = 0; dst < 7; ++dst) {
            if (!isStrategicTableauMove(state.tableau, src, first, dst, card)) continue;
            const Move move{VirtualMoveKind::Tableau, src, dst, first};
            if (!physicalCandidateAllowed(state, move, nowMs())) continue;
            if (executeTrackedEmergencyMove(localGs, state, move, 160000,
                                            "tableau-reveal")) {
                return true;
            }
        }
    }

    // The strategic planner intentionally records only lines that prove a
    // reveal, safe Foundation gain, debt reduction, or a strong recovery
    // preparation.  A rare live position can still have legal preparatory
    // moves after every such branch is exhausted.  Rank those moves with the
    // existing virtual look-ahead instead of parking the bot or choosing one
    // greedily.  This block is reached only after normal planning and all
    // checkpoint recovery have failed, so it cannot disturb the strong common
    // path that already wins most games.
    struct EmergencyCandidate {
        Move move;
        int preliminary = -10000000;
        int future = -10000000;
        std::string resultKey;
    };
    const std::string rootKey = virtualPositionKey(state);
    std::vector<Move> legal;
    const bool perfectRecovery = knownCardCount(state) == 52 && !hasOpaqueCards(state);
    generateVirtualMoves(state, legal, perfectRecovery);
    std::vector<EmergencyCandidate> candidates;
    candidates.reserve(legal.size());
    for (const Move& move : legal) {
        if (move.kind == VirtualMoveKind::Talon) continue;
        const std::string edgeKey = moveKey(move);
        const auto triedAtRoot = g_emergencyEdgesTried.find(rootKey);
        if ((triedAtRoot != g_emergencyEdgesTried.end() &&
             triedAtRoot->second.find(edgeKey) != triedAtRoot->second.end()) ||
            rootMoveFailed(rootKey, move) ||
            !physicalCandidateAllowed(state, move, nowMs())) {
            continue;
        }
        GameState after = state;
        if (!applyVirtualMove(after, move)) continue;
        const std::string resultKey = virtualPositionKey(after);
        // Exact repetition is a proven motion loop, not a useful rescue.
        if (seenRecentSolverState(resultKey, nowMs())) continue;
        if (g_deadStateCache.find(solverPositionKey(after)) != g_deadStateCache.end())
            continue;
        const int preliminary = virtualStructuralScore(after) +
                                candidateTacticalBonus(state, after, move) +
                                immediateMoveTier(state, move) * 110 -
                                strategicStatePenalty(solverPositionKey(after));
        candidates.push_back({move, preliminary, -10000000, resultKey});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const EmergencyCandidate& a, const EmergencyCandidate& b) {
                  return a.preliminary > b.preliminary;
              });

    // Share one small deadline among the best candidates.  Rescue search is
    // deliberately bounded because this runs on the Unity game thread.
    const uint64_t rescueDeadline = nowMs() + 20;
    const int evaluateCount = std::min(8, (int)candidates.size());
    for (int i = 0; i < evaluateCount && nowMs() < rescueDeadline; ++i) {
        const int depth = hiddenCardCount(state) <= 7 ? 20 : 16;
        candidates[i].future = scoreFutureCandidate(
            state, candidates[i].move, depth, 480, rescueDeadline,
            perfectRecovery);
        if (candidates[i].future > -8000000)
            candidates[i].future += immediateMoveTier(state, candidates[i].move) * 140;
    }
    std::sort(candidates.begin(), candidates.begin() + evaluateCount,
              [](const EmergencyCandidate& a, const EmergencyCandidate& b) {
                  if (a.future != b.future) return a.future > b.future;
                  return a.preliminary > b.preliminary;
              });
    for (int i = 0; i < evaluateCount; ++i) {
        if (candidates[i].future <= -8000000) continue;
        if (executeTrackedEmergencyMove(localGs, state, candidates[i].move,
                                        candidates[i].future,
                                        "searched-preparation")) {
            SOL_LOG("emergency searched-choice rank=%d/%d preliminary=%d result=%s",
                    i + 1, evaluateCount, candidates[i].preliminary,
                    candidates[i].resultKey.c_str());
            return true;
        }
    }

    // A completely explored planning frontier must never freeze with stock
    // cards still physically drawable. Advance to the next *new* Talon state;
    // exact-state history blocks a useless full recycle, so this escape hatch
    // cannot become the old infinite stock loop.
    if (!state.talon.empty() || !state.waste.empty()) {
        const Move talonMove{VirtualMoveKind::Talon, -1, -1, -1};
        GameState next = state;
        if (applyVirtualMove(next, talonMove) &&
            physicalCandidateAllowed(state, talonMove, nowMs()) &&
            executeTrackedEmergencyMove(localGs, state, talonMove, -500000,
                                        "talon-new-state")) {
            SOL_LOG("emergency talon advance stock=%d waste=%d",
                    (int)state.talon.size(), (int)state.waste.size());
            return true;
        }
    }
    return false;
}

static void logStalledExpertState(const GameState& state, uint64_t now) {
    if (now - g_lastStallDiagMs < 2500) return;
    g_lastStallDiagMs = now;
    std::string tableauTops;
    std::string foundationTops;
    for (int i = 0; i < 7; ++i) {
        if (!tableauTops.empty()) tableauTops += ',';
        if (state.tableau[i].empty()) tableauTops += "-";
        else tableauTops += std::to_string(state.tableau[i].back().id) + ":" +
                            std::to_string(state.tableau[i].back().value);
    }
    for (int i = 0; i < 4; ++i) {
        if (!foundationTops.empty()) foundationTops += ',';
        if (state.foundations[i].empty()) foundationTops += "-";
        else foundationTops += std::to_string(state.foundations[i].back().id) + ":" +
                               std::to_string(state.foundations[i].back().value);
    }
    std::vector<Move> legal;
    generateVirtualMoves(state, legal);
    const int wasteId = state.waste.empty() ? -1 : state.waste.back().id;
    const int wasteValue = state.waste.empty() ? -1 : state.waste.back().value;
    SOL_LOG("expert stalled recycle=%d draw=%d stock=%d waste=%d top=%d:%d moves=%d frontier=%d roots=%d risky=%d recoveryRoots=%d recoveryMode=%d committed=%d debt=%d tableau=[%s] foundation=[%s]",
            state.recycles, state.drawCount, (int)state.talon.size(), (int)state.waste.size(),
            wasteId, wasteValue, (int)legal.size(),
            (int)g_persistentPlanner.frontier.size(),
            (int)g_persistentPlanner.roots.size(),
            (int)g_persistentPlanner.riskyRoots.size(),
            (int)g_persistentPlanner.recoveryRoots.size(), g_recoveryMode ? 1 : 0,
            (int)g_committedPlan.size(), foundationRiskDebt(state.foundations),
            tableauTops.c_str(), foundationTops.c_str());
}

static int immediateMoveTier(const GameState& state, const Move& move) {
    if (move.kind == VirtualMoveKind::FoundationToTableau) return -100;
    if (move.kind == VirtualMoveKind::Talon) return 0;
    if (move.kind == VirtualMoveKind::Foundation) {
        const Card* card = nullptr;
        if (move.from < 0) {
            if (!state.waste.empty()) card = &state.waste.back();
        } else if (move.from < 7 && !state.tableau[move.from].empty()) {
            card = &state.tableau[move.from].back();
        }
        if (!card) return -100;
        if (move.from >= 0 && state.tableau[move.from].size() > 1 &&
            !state.tableau[move.from][state.tableau[move.from].size() - 2].faceUp())
            return 110; // foundation advance that immediately uncovers a card
        // Aces/twos are unconditional progress; other safe foundation moves
        // remain above stock cycling but below revealing a hidden card.
        if (cardRank(*card) <= 1) return 90;
        if (foundationAdvanceIsSafe(*card, state.foundations)) return 70;
        // A legal exposed waste card may need to move upward to uncover the
        // next draw-3 card. Keep it available as a rescue, not forbidden.
        return move.from < 0 ? 50 : 0;
    }
    if (move.from < 0) return 80; // playable waste card must not be skipped.
    if (move.from >= 7 || move.to < 0 || move.to >= 7 || move.start < 0 ||
        move.start >= (int)state.tableau[move.from].size()) return -100;
    const bool revealsHidden = move.start > 0 && !state.tableau[move.from][move.start - 1].faceUp();
    if (revealsHidden) return 100;
    if (improvesNextPosition(state.tableau, state.foundations, move.from, move.to,
                             state.tableau[move.from][move.start].id)) return 60;
    return 0;
}

static bool chooseAndExecuteExpertMove(void* localGs, const GameState& state) {
    const uint64_t now = nowMs();
    const std::string rootKey = virtualPositionKey(state);

    // Validate the previously submitted asynchronous Unity action. If the
    // board never reached its simulated result, blacklist that root edge and
    // let the retained alternatives take over instead of retrying forever.
    if (g_waitingForMoveResult) {
        if (rootKey == g_pendingExpectedKey) {
            g_waitingForMoveResult = false;
            const uint64_t recoveryStep = pacingRecoveryStepMs();
            g_pacingBackoffMs = g_pacingBackoffMs > recoveryStep
                ? g_pacingBackoffMs - recoveryStep
                : 0;
        } else if (rootKey == g_pendingRootKey) {
            if (now - g_pendingMoveAt < SOL_ACTION_RESULT_TIMEOUT_MS) return false;
            // The native action never changed the board. Restore our derived
            // consecutive-recycle counter as well, otherwise a failed click
            // can fabricate a Talon lock that does not exist physically.
            g_actualRecycleCount = g_pendingRecycleCountBefore;
            g_pacingBackoffMs = std::min(
                SOL_MAX_PACING_BACKOFF_MS,
                g_pacingBackoffMs + SOL_PACING_BACKOFF_STEP_MS);
            registerPhysicalRootFailure(g_pendingRootKey, g_pendingMoveKey);
            SOL_LOG("expert action failed root=%s move=%s: using backup pacing=%llu",
                    g_pendingRootKey.c_str(), g_pendingMoveKey.c_str(),
                    (unsigned long long)g_pacingBackoffMs);
            g_waitingForMoveResult = false;
            g_committedPlan.clear();
        } else {
            // The game changed to a legitimate state different from the exact
            // visual model (animation/order variation). Replan from reality.
            g_waitingForMoveResult = false;
            g_committedPlan.clear();
        }
    }

    // A fully exhausted state is parked briefly, not forever. The emergency
    // path remains active during the cooldown; afterwards the deepest bounded
    // recovery pass is reopened. This prevents a solvable board from becoming
    // permanently idle because one finite search slice ran out of branches.
    if (!g_exhaustedRecoveryKey.empty()) {
        if (rootKey == g_exhaustedRecoveryKey) {
            if (now - g_exhaustedRecoveryAt < SOL_EXHAUSTED_RETRY_MS) return false;
            g_exhaustedRecoveryKey.clear();
            g_exhaustedRecoveryAt = 0;
            g_recoveryMode = true;
            g_recoveryCheckpointId = 0;
            g_lastRecoveryAt = now;
            g_progressTracker.recoveryAttemptsAtState = 0;
            g_progressTracker.stallPending = false;
            g_progressTracker.stallReason.clear();
            SOL_LOG("[RECOVERY] exhausted-state cooldown elapsed; reopening bounded search escalation=%d",
                    g_recoveryEscalationByState[rootKey]);
        } else {
            g_exhaustedRecoveryKey.clear();
            g_exhaustedRecoveryAt = 0;
        }
    }

    detectPlannerIdleStall(rootKey, now);
    if (g_progressTracker.stallPending) {
        // Do not abort a route one step before a proven reveal/foundation
        // gain. Otherwise the checkpointed branch is blamed and the current
        // physical state is replanned with its failed structure penalised.
        if (!g_committedPlan.empty() &&
            moveCreatesIrreversibleProgress(state, g_committedPlan.front())) {
            g_progressTracker.stallPending = false;
            g_progressTracker.stallReason.clear();
        } else {
            activateStrategicRecovery(state, rootKey);
        }
    }

    // Continue the verified multi-step route found at the previous root.  A
    // plan is committed only up to its first proven progress milestone; every
    // physical step is still validated against the freshly-read LocalGs.
    if (!g_committedPlan.empty()) {
        const Move nextMove = g_committedPlan.front();
        const std::vector<Move> continuation = g_committedPlan;
        GameState expected = state;
        const int recycleCountBefore = g_actualRecycleCount;
        if (physicalCandidateAllowed(state, nextMove, now) &&
            applyVirtualMove(expected, nextMove) &&
            g_deadStateCache.find(solverPositionKey(expected)) == g_deadStateCache.end() &&
            executeExpertMove(localGs, state, nextMove, g_committedPlanScore)) {
            saveDecisionCheckpoint(state, nextMove, continuation,
                                   legalAlternativeKeys(state, nextMove),
                                   g_committedPlanScore);
            g_committedPlan.erase(g_committedPlan.begin());
            g_pendingRootKey = rootKey;
            g_pendingExpectedKey = virtualPositionKey(expected);
            g_pendingMoveKey = moveKey(nextMove);
            g_pendingMoveAt = now;
            g_pendingRecycleCountBefore = recycleCountBefore;
            g_waitingForMoveResult = true;
            SOL_LOG("expert committed move=%s remaining=%d score=%d",
                    g_pendingMoveKey.c_str(), (int)g_committedPlan.size(),
                    g_committedPlanScore);
            return true;
        }
        SOL_LOG("expert committed route invalidated remaining=%d: replanning",
                (int)g_committedPlan.size());
        g_committedPlan.clear();
    }

    if (g_persistentPlanner.rootKey != rootKey)
        initializePersistentRoot(state, rootKey);
    advancePersistentSearch(state);
    detectPlannerIdleStall(rootKey, now);
    if (g_progressTracker.stallPending &&
        activateStrategicRecovery(state, rootKey)) {
        // A retained current-root frontier remains valid; give its newly
        // recovery-weighted siblings one bounded slice immediately.
        advancePersistentSearch(state);
    }

    const int requiredHorizon = requiredRootHorizon(g_persistentPlanner);
    const int bestHorizon = bestExecutableRootHorizon(rootKey);
    const bool horizonReady = !g_persistentPlanner.perfectInformation ||
                              bestHorizon >= requiredHorizon;
    const bool normalBudgetReady = !g_persistentPlanner.endgame &&
        g_persistentPlanner.frames >= SOL_NORMAL_SEARCH_FRAMES &&
        g_persistentPlanner.expanded >= 1500;
    const bool endgameBudgetReady = g_persistentPlanner.endgame &&
        g_persistentPlanner.frames >= SOL_ENDGAME_SEARCH_FRAMES &&
        g_persistentPlanner.expanded >= 5500;
    // Full-deal information earns a short survival proof before execution.
    // A bounded fallback prevents the proof gate itself from looking like a
    // freeze on a wide early-game tree.
    const bool boundedProofFallback = !g_persistentPlanner.endgame ?
        (g_persistentPlanner.frames >= 8 || g_persistentPlanner.expanded >= 5000) :
        (g_persistentPlanner.frames >= 13 || g_persistentPlanner.expanded >= 12000);
    const bool ready = g_persistentPlanner.solved ||
                       (g_persistentPlanner.foundProgress &&
                        (((normalBudgetReady || endgameBudgetReady) && horizonReady) ||
                          boundedProofFallback)) ||
                       g_persistentPlanner.frontier.empty();
    // If no conservative line exists, keep searching substantially longer
    // before accepting a foundation-only debt increase.  This is the exact
    // fallback needed for positions where a legal 5-on-4 must move to expose
    // the rest of the game, without letting normal play rush one suit upward.
    const bool riskyReady = g_persistentPlanner.roots.empty() &&
                            !g_persistentPlanner.riskyRoots.empty() &&
                            (g_persistentPlanner.frontier.empty() ||
                             (!g_persistentPlanner.endgame &&
                              (g_persistentPlanner.frames >= 32 ||
                               g_persistentPlanner.expanded >= 14000)) ||
                             (g_persistentPlanner.endgame &&
                              (g_persistentPlanner.frames >= 60 ||
                               g_persistentPlanner.expanded >= 30000)));
    const bool recoveryReady = g_recoveryMode && g_persistentPlanner.roots.empty() &&
                               !g_persistentPlanner.recoveryRoots.empty() &&
                               (g_persistentPlanner.frontier.empty() ||
                                (!g_persistentPlanner.endgame &&
                                 (g_persistentPlanner.frames >= 18 ||
                                  g_persistentPlanner.expanded >= 9000)) ||
                                (g_persistentPlanner.endgame &&
                                 (g_persistentPlanner.frames >= 36 ||
                                  g_persistentPlanner.expanded >= 18000)));

    enum class RootSource { Normal, Risky, Recovery };
    RootSource rootSource = RootSource::Normal;
    std::vector<RootPlan> alternatives;
    if (ready && !g_persistentPlanner.roots.empty()) {
        alternatives.reserve(g_persistentPlanner.roots.size());
        for (const auto& entry : g_persistentPlanner.roots)
            alternatives.push_back(entry.second);
    } else if (recoveryReady) {
        rootSource = RootSource::Recovery;
        alternatives.reserve(g_persistentPlanner.recoveryRoots.size() +
                             (riskyReady ? g_persistentPlanner.riskyRoots.size() : 0));
        for (const auto& entry : g_persistentPlanner.recoveryRoots)
            alternatives.push_back(entry.second);
        if (riskyReady)
            for (const auto& entry : g_persistentPlanner.riskyRoots)
                alternatives.push_back(entry.second);
    } else if (riskyReady) {
        rootSource = RootSource::Risky;
        alternatives.reserve(g_persistentPlanner.riskyRoots.size());
        for (const auto& entry : g_persistentPlanner.riskyRoots)
            alternatives.push_back(entry.second);
    } else {
        if (g_persistentPlanner.frontier.empty()) {
            flagStrategicStall(g_recoveryMode ? "recovery-search-exhausted" :
                                                "planner-frontier-exhausted");
            activateStrategicRecovery(state, rootKey);
        }
        return false;
    }

    struct RankedPlan {
        RootPlan plan;
        int effective = -10000000;
        int horizon = 0;
        int terminalLeaves = 0;
    };
    std::vector<RankedPlan> ranked;
    ranked.reserve(alternatives.size());
    for (const RootPlan& plan : alternatives) {
        if (rootMoveFailed(rootKey, plan.first)) continue;
        const int horizon = rootHorizonFor(plan.first);
        const int terminalLeaves = rootTerminalLeavesFor(plan.first);
        int effective = plan.score + immediateMoveTier(state, plan.first) * 90 -
                        (int)plan.line.size() * 12 -
                        strategicLinePenalty(state, plan.line);
        effective += std::min(horizon, requiredHorizon + 8) * 1300;
        effective -= std::min(terminalLeaves, 20) * 750;
        if (g_persistentPlanner.perfectInformation &&
            horizon < requiredHorizon && !g_persistentPlanner.frontier.empty())
            effective -= (requiredHorizon - horizon) * 9000;
        ranked.push_back({plan, effective, horizon, terminalLeaves});
    }
    std::sort(ranked.begin(), ranked.end(), [](const RankedPlan& a,
                                                const RankedPlan& b) {
        return a.effective > b.effective;
    });

    for (const RankedPlan& rankedPlan : ranked) {
        const RootPlan& plan = rankedPlan.plan;
        if (!physicalCandidateAllowed(state, plan.first, now)) {
            g_failedStrategicBranches[rootKey].insert(moveKey(plan.first));
            discardPlannerRootEdge(rootKey, moveKey(plan.first));
            continue;
        }
        const bool allowRiskyFoundation = rootSource != RootSource::Normal;
        const CertifiedPlanPrefix certified = certifyPlanPrefix(
            state, plan.line, allowRiskyFoundation, true,
            rootSource == RootSource::Recovery);
        if (!certified.valid) {
            // Do not blacklist the whole first edge for a bad *deeper* route;
            // another continuation below the same legal root may still win.
            // Removing only the published plan lets the retained frontier find
            // that sibling while the offending structure receives a penalty.
            discardPlannerRootEdge(rootKey, moveKey(plan.first));
            if (!certified.failureStateKey.empty()) {
                int& failures = g_strategicStateFailures[certified.failureStateKey];
                failures = std::min(5, failures + 1);
            }
            SOL_LOG("[PLAN_GUARD] rejected move=%s reason=%s step=%d line=%d",
                    moveKey(plan.first).c_str(), certified.reason.c_str(),
                    certified.failureIndex, (int)plan.line.size());
            continue;
        }
        std::vector<Move> executableLine(
            plan.line.begin(), plan.line.begin() + certified.progressIndex + 1);
        GameState expected = state;
        if (!applyVirtualMove(expected, plan.first)) continue;
        if (g_deadStateCache.find(solverPositionKey(expected)) != g_deadStateCache.end()) continue;
        const int recycleCountBefore = g_actualRecycleCount;
        if (!executeExpertMove(localGs, state, plan.first, plan.score)) {
            registerPhysicalRootFailure(rootKey, moveKey(plan.first));
            continue;
        }
        std::vector<std::string> checkpointAlternatives;
        checkpointAlternatives.reserve(ranked.size());
        for (const RankedPlan& other : ranked) {
            const std::string otherKey = moveKey(other.plan.first);
            if (otherKey != moveKey(plan.first) &&
                !rootMoveFailed(rootKey, other.plan.first))
                checkpointAlternatives.push_back(otherKey);
        }
        saveDecisionCheckpoint(state, plan.first, executableLine,
                               std::move(checkpointAlternatives), plan.score);
        g_committedPlan.clear();
        if (executableLine.size() > 1)
            g_committedPlan.assign(executableLine.begin() + 1,
                                   executableLine.end());
        g_committedPlanScore = plan.score;
        g_pendingRootKey = rootKey;
        g_pendingExpectedKey = virtualPositionKey(expected);
        g_pendingMoveKey = moveKey(plan.first);
        g_pendingMoveAt = now;
        g_pendingRecycleCountBefore = recycleCountBefore;
        g_waitingForMoveResult = true;
        const bool recovered = g_recoveryMode;
        if (recovered) {
            SOL_LOG("[RECOVERY_RESULT] selectedBranch=%s searchNodes=%d newPlanLength=%d",
                    g_pendingMoveKey.c_str(), g_persistentPlanner.expanded,
                    (int)executableLine.size());
            g_recoveryMode = false;
            g_recoveryCheckpointId = 0;
            g_recoveryGraceTransitions = 4;
        }
        g_progressTracker.stallPending = false;
        g_progressTracker.stallReason.clear();
        SOL_LOG("expert selected move=%s score=%d effective=%d line=%d/%d milestone=%s horizon=%d/%d terminal=%d known=%d/52 expanded=%d frames=%d backup=%d source=%d",
                g_pendingMoveKey.c_str(), plan.score, rankedPlan.effective,
                (int)executableLine.size(), (int)plan.line.size(),
                certified.reason.c_str(), rankedPlan.horizon, requiredHorizon,
                rankedPlan.terminalLeaves, g_persistentPlanner.rootKnownCards,
                g_persistentPlanner.expanded, g_persistentPlanner.frames,
                (int)ranked.size() - 1, (int)rootSource);
        return true;
    }
    if (g_persistentPlanner.frontier.empty()) {
        flagStrategicStall("no-executable-root-alternative");
        activateStrategicRecovery(state, rootKey);
    }
    return false;
}

static bool attemptFoundationMoves(void* localGs,
                                   const std::array<std::vector<Card>, 7>& tableau,
                                   const std::vector<Card>& talon,
                                   const std::vector<Card>& waste,
                                   const std::array<std::vector<Card>, 4>& foundations) {
    // Only submit moves that satisfy the exact rules used by the original
    // SolitaireRaycaster. OnMoveCard itself queues an action, so its return
    // value is not a legal-move verdict and must never be used as one.
    // The exposed waste card is the only card that disappears on the next
    // talon draw. Always test it first, so a legal 2♠ over A♠ (and any other
    // legal foundation advance) is never skipped for another talon cycle.
    const GameState future = makeGameState(tableau, foundations, talon, waste);
    if (!waste.empty() && waste.back().faceUp()) {
        for (int f = 0; f < 4; ++f) {
            const Card* top = foundations[f].empty() ? nullptr : &foundations[f].back();
            if (!canDropOnFoundation(waste.back(), top)) continue;
            if (!foundationAdvanceIsSafe(waste.back(), foundations)) continue;
            if (!simulate_future_moves(future, {VirtualMoveKind::Foundation, -1, f, -1}, 18)) continue;
            if (moveCard(localGs, waste.back(), g_stackPrefix + "waste",
                         foundationName(f), (int)foundations[f].size())) {
                g_rrSource = 0;
                SOL_LOG("waste-foundation id=%d foundation=%d", waste.back().id, f);
                return true;
            }
        }
    }

    std::array<std::pair<std::string, Card>, 8> sources{};
    for (int i = 0; i < 7; ++i) {
        if (!tableau[i].empty() && tableau[i].back().faceUp()) sources[i] = {tableauName(i), tableau[i].back()};
    }
    // Waste was handled above with strict priority; only tableau cards remain
    // in this pass.

    for (int step = 0; step < 7; ++step) {
        const int src = (g_rrSource + step) % 7;
        if (!sources[src].second.valid()) continue;
        for (int f = 0; f < 4; ++f) {
            const Card* foundationTop = foundations[f].empty() ? nullptr : &foundations[f].back();
            if (!canDropOnFoundation(sources[src].second, foundationTop)) continue;
            if (!foundationAdvanceIsSafe(sources[src].second, foundations)) continue;
            if (!simulate_future_moves(future, {VirtualMoveKind::Foundation, src, f, -1}, 18)) continue;
            if (moveCard(localGs, sources[src].second, sources[src].first,
                         foundationName(f), (int)foundations[f].size())) {
                g_rrSource = (src + 1) % 8;
                return true;
            }
        }
    }

    return false;
}

static bool attemptTableauMoves(void* localGs,
                                const std::array<std::vector<Card>, 7>& tableau,
                                const std::vector<Card>& talon,
                                const std::vector<Card>& waste,
                                const std::array<std::vector<Card>, 4>& foundations) {
    // The original state method validates every drop.  We only offer exposed
    // top cards, preferring waste first, then rotating tableau columns to keep
    // the play natural and to expose closed cards early.
    const GameState future = makeGameState(tableau, foundations, talon, waste);
    std::array<std::pair<std::string, Card>, 8> sources{};
    if (!waste.empty()) sources[0] = {g_stackPrefix + "waste", waste.back()};
    for (int i = 0; i < 7; ++i) {
        if (!tableau[i].empty()) {
            sources[i + 1] = {tableauName(i), firstMovableTableauCard(localGs, tableau[i])};
        }
    }

    // First pass: free a closed card. This is the strongest practical move in
    // Klondike and avoids harmless rearrangements while useful cards remain.
    for (int revealOnly = 1; revealOnly >= 0; --revealOnly) {
        for (int step = 0; step < 8; ++step) {
            const int src = (g_rrSource + step) % 8;
            const auto& source = sources[src];
            if (!source.second.faceUp()) continue;
            const bool revealsClosed = src > 0 && !tableau[src - 1].empty() &&
                                       !tableau[src - 1].front().faceUp();
            if (revealOnly && !revealsClosed) continue;
            for (int dst = 0; dst < 7; ++dst) {
                if (source.first == tableauName(dst)) continue;
                const Card* destinationTop = tableau[dst].empty() ? nullptr : &tableau[dst].back();
                if (!canDropOnTableau(source.second, destinationTop)) continue;
                if (src > 0) {
                    const int start = movableCardIndex(tableau[src - 1], source.second.id);
                    if (!isStrategicTableauMove(tableau, src - 1, start, dst, source.second)) continue;
                } else if (tableau[dst].empty() && cardRank(source.second) != 12) {
                    continue;
                }
                // A non-reveal tableau move is permitted only when a one-ply
                // simulation proves it creates an additional useful move.
                if (!revealOnly && src > 0 &&
                    !improvesNextPosition(tableau, foundations, src - 1, dst, source.second.id)) continue;
                if (src > 0 && wouldRepeatRecentTableau(tableau, src - 1, dst,
                                                         source.second.id, nowMs())) continue;
                const int start = src > 0 ? movableCardIndex(tableau[src - 1], source.second.id) : -1;
                const Move candidate{VirtualMoveKind::Tableau, src > 0 ? src - 1 : -1, dst, start};
                if (!simulate_future_moves(future, candidate, 18)) continue;
                if (moveCard(localGs, source.second, source.first, tableauName(dst),
                             (int)tableau[dst].size())) {
                    g_rrSource = (src + 1) % 8;
                    return true;
                }
            }
        }
    }

    // A valid tableau drag may start in the middle of an exposed sequence,
    // not only at its first face-up card.  Use these moves only as a fallback
    // and only when simulation proves they create a new productive option.
    for (int step = 0; step < 7; ++step) {
        const int src = (g_rrSource + step) % 7;
        const int first = firstFaceUpIndex(tableau[src]);
        if (first < 0) continue;
        for (int start = first + 1; start < (int)tableau[src].size(); ++start) {
            const Card& moving = tableau[src][start];
            if (!moving.faceUp()) continue;
            for (int dst = 0; dst < 7; ++dst) {
                if (src == dst) continue;
                const Card* destinationTop = tableau[dst].empty() ? nullptr : &tableau[dst].back();
                if (!canDropOnTableau(moving, destinationTop)) continue;
                if (!isStrategicTableauMove(tableau, src, start, dst, moving)) continue;
                if (!improvesNextPosition(tableau, foundations, src, dst, moving.id)) continue;
                if (wouldRepeatRecentTableau(tableau, src, dst, moving.id, nowMs())) continue;
                if (!simulate_future_moves(future, {VirtualMoveKind::Tableau, src, dst, start}, 18)) continue;
                if (moveCard(localGs, moving, tableauName(src), tableauName(dst),
                             (int)tableau[dst].size())) {
                    g_rrSource = (src + 1) % 8;
                    SOL_LOG("substack-move id=%d from=%d to=%d", moving.id, src, dst);
                    return true;
                }
            }
        }
    }

    RescuePlan rescue;
    if (findRescuePlan(tableau, foundations, rescue) &&
        !wouldRepeatRecentTableau(tableau, rescue.from, rescue.to, rescue.cardId, nowMs())) {
        const int index = movableCardIndex(tableau[rescue.from], rescue.cardId);
        if (index >= 0 && !simulate_future_moves(future,
                {VirtualMoveKind::Tableau, rescue.from, rescue.to, index}, 18)) return false;
        if (index >= 0 && moveCard(localGs, tableau[rescue.from][index], tableauName(rescue.from),
                                   tableauName(rescue.to), (int)tableau[rescue.to].size())) {
            g_rrSource = (rescue.from + 1) % 8;
            SOL_LOG("beam-rescue id=%d from=%d to=%d score=%d", rescue.cardId,
                    rescue.from, rescue.to, rescue.score);
            return true;
        }
    }
    return false;
}

} // namespace

void solitaire_set_performance_profile(int profile, int sliceNodes, int sliceMs,
                                       int arenaNodes, int cardSafetyMs,
                                       int talonSafetyMs) {
    // Accept only bounded planner budgets from the signed loader. Device ULTRA
    // uses a compact slice; PC TURBO gets a wider slice. We deliberately do not
    // copy the loader's raw card/talon delays: the proven 4/2 ms minimum plus
    // exact-state ACK and adaptive backoff is safer and faster on both targets.
    const int safeProfile = std::clamp(profile, 0, 3);
    const int safeNodes = std::clamp(sliceNodes, 700, 6000);
    const int safeSliceMs = std::clamp(sliceMs, 3, 13);
    const int safeArena = std::clamp(arenaNodes, 12000, 64000);
    g_performanceProfile.store(safeProfile, std::memory_order_relaxed);
    g_searchSliceNodes.store(safeNodes, std::memory_order_relaxed);
    g_searchSliceMs.store(safeSliceMs, std::memory_order_relaxed);
    g_searchArenaNodes.store(safeArena, std::memory_order_relaxed);
    SOL_LOG("[PERFORMANCE] applied profile=%d nodes=%d sliceMs=%d arena=%d physical=4/2 uiWatchdog=%llu/%llu requestedPhysical=%d/%d exactAck=1",
             safeProfile, safeNodes, safeSliceMs, safeArena,
             (unsigned long long)(safeProfile == 2 ? SOL_PHONE_CARD_UI_WATCHDOG_MS
                                                   : SOL_UI_UPDATE_WATCHDOG_MS),
             (unsigned long long)(safeProfile == 2 ? SOL_PHONE_TALON_UI_WATCHDOG_MS
                                                   : SOL_UI_UPDATE_WATCHDOG_MS),
             cardSafetyMs, talonSafetyMs);
}

void solitaire_init(uintptr_t il2cppBase) {
    if (!il2cppBase) return;
    // Loader builds cannot assume libil2cpp exports are visible through dlsym.
    // Keep the current resolver integration around the restored strong solver.
    g_stringNew = reinterpret_cast<StringNewFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_string_new"));
    g_objectToString = reinterpret_cast<ObjectToStringFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_object_to_string"));
    g_classGetName = reinterpret_cast<ClassNameFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_class_get_name"));
    g_domainGet = reinterpret_cast<DomainGetFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_domain_get"));
    g_domainAssemblies = reinterpret_cast<DomainAssembliesFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_domain_get_assemblies"));
    g_assemblyImage = reinterpret_cast<AssemblyImageFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_assembly_get_image"));
    g_imageName = reinterpret_cast<ImageNameFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_image_get_name"));
    g_classFromName = reinterpret_cast<ClassFromNameFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_class_from_name"));
    g_staticData = reinterpret_cast<StaticDataFn>(goat_resolve_il2cpp_export(il2cppBase, "il2cpp_class_get_static_field_data"));
    g_getUi = (GetUiFn)(il2cppBase + RVA_SOL_UI_GET_INSTANCE);
    g_getStackCards = (GetStackCardsFn)(il2cppBase + RVA_SOL_GET_STACK_CARDS);
    g_moveCard = (MoveCardFn)(il2cppBase + RVA_SOL_MOVE_CARD);
    g_talonClicked = (TalonClickedFn)(il2cppBase + RVA_SOL_TALON_CLICKED);
    g_talonUi = (TalonUiFn)(il2cppBase + RVA_SOL_UI_TALON_CLICK);
    g_syncPlayerCardsMapping = (SyncPlayerCardsMappingFn)(
        il2cppBase + RVA_SOL_SYNC_PLAYER_CARDS_MAPPING);
    g_rayMove = (RayMoveFn)(il2cppBase + RVA_SOL_RAY_MOVE_CARD);
    static const bool planGuardSelfTestsPassed = runPlanGuardSelfTests();
    SOL_LOG("init str=%p ui=%p stacks=%p talon=%p talonUi=%p mapSync=%p rayMove=%p domain=%p assemblies=%p image=%p class=%p static=%p",
            g_stringNew, g_getUi, g_getStackCards, g_talonClicked, g_talonUi,
            g_syncPlayerCardsMapping, g_rayMove,
            g_domainGet, g_domainAssemblies, g_assemblyImage, g_classFromName, g_staticData);
    if (!planGuardSelfTestsPassed)
        SOL_LOG("[PLAN_GUARD_TEST] FAILED: candidate guard must remain conservative");
}

void solitaire_capture_ui(void* solitaireUi) {
    if (!tryEnterSolitaire()) return;
    SolitaireAccessGuard guard;
    if (solitaireUi && g_ui != solitaireUi) {
        g_ui = solitaireUi;
        g_lastLocalGs = nullptr;
        resetRoundState(true);
        SOL_LOG("captured UI=%p", g_ui);
    }
}

void solitaire_set_enabled(bool enabled) {
    if (enabled && !license_security_allows_game(GOAT_GAME_SOLITAIRE)) enabled = false;
    if (!tryEnterSolitaire()) return;
    SolitaireAccessGuard guard;
    g_enabled = enabled;
    resetRoundState(false);
    // The overlay toggle is delivered on Android's UI thread. Unity objects are
    // reacquired by capture_ui/tick on UnityMain to avoid intermittent first-
    // enable races while preserving the strong solver unchanged.
    SOL_LOG("auto=%d ready=%d ui=%p", enabled ? 1 : 0, solitaire_is_ready() ? 1 : 0, g_ui);
}

bool solitaire_is_enabled() { return g_enabled; }

bool solitaire_is_ready() {
    return g_ui && g_stringNew && g_getStackCards && g_talonUi && g_rayMove;
}

void solitaire_tick() {
    if (!g_enabled) return;
    // The solver itself is gated, not only its UI toggle. A patched button or
    // stale memory flag therefore cannot run Auto Solitaire without a current
    // signed server lease.
    if (!license_security_allows_game(GOAT_GAME_SOLITAIRE)) {
        g_enabled = false;
        return;
    }
    if (!tryEnterSolitaire()) return;
    SolitaireAccessGuard guard;
    if (!g_ui && g_getUi) {
        // capture_ui also uses the same guard, so capture directly while the
        // tick owns access rather than re-entering through its public path.
        void* ui = g_getUi(nullptr);
        if (ui && g_ui != ui) {
            g_ui = ui;
            g_lastLocalGs = nullptr;
            resetRoundState(true);
            SOL_LOG("captured UI=%p", g_ui);
        }
    }
    const uint64_t now = nowMs();
    if (!solitaire_is_ready()) {
        if (now - g_lastDiagMs > 2500) {
            g_lastDiagMs = now;
            SOL_LOG("waiting ready ui=%p str=%p stack=%p rayMove=%p talonUi=%p", g_ui,
                    g_stringNew, g_getStackCards, g_rayMove, g_talonUi);
        }
        return;
    }
    void* localGs = *(void**)((uint8_t*)g_ui + OFF_SOL_UI_LOCAL_GS);
    if (!localGs) {
        if (now - g_lastDiagMs > 2500) {
            g_lastDiagMs = now;
            SOL_LOG("waiting LocalGs ui=%p", g_ui);
        }
        return;
    }
    if (localGs != g_lastLocalGs) {
        g_lastLocalGs = localGs;
        resetRoundState(true);
        SOL_LOG("new solitaire round gs=%p", localGs);
    }
    const int configuredDrawCount = *(int*)((uint8_t*)localGs + OFF_SOL_CARDS_PER_TALON_FLIP);
    if (configuredDrawCount >= 1 && configuredDrawCount <= 3 &&
        configuredDrawCount != g_cardsPerTalonFlip) {
        g_cardsPerTalonFlip = configuredDrawCount;
        clearPlannerFrontier();
        SOL_LOG("using LocalGs cards-per-flip=%d", g_cardsPerTalonFlip);
    }
    logNativeStackKeys(localGs);
    discoverStackPrefix(localGs);
    logHiddenStateSnapshot(localGs);
    logFullSnapshot(localGs);

    // Talon flips settle sooner than card drags. This only shortens visual wait;
    // chooseAndExecuteExpertMove still validates g_pendingExpectedKey and will
    // not release a second action until the physical board is synchronized.
    const uint64_t settleMs = g_lastActionWasTalon
        ? SOL_TALON_ACTION_SETTLE_MS + g_pacingBackoffMs
        : SOL_CARD_ACTION_SETTLE_MS + g_pacingBackoffMs;
    if (g_lastMoveMs != 0 && now - g_lastMoveMs < settleMs) return;
    // Use the game's own UI synchronization flag instead of a conservative
    // fixed sleep. This releases the next search/action on the first safe
    // frame, while a bounded watchdog prevents a stale flag from freezing a
    // round forever.
    const bool uiUpdating =
        *(const uint8_t*)((const uint8_t*)localGs + OFF_SOL_IS_UPDATING_UI) != 0;
    if (uiUpdating && g_lastMoveMs != 0 &&
        now - g_lastMoveMs < effectiveUiWatchdogMs()) return;

    std::array<std::vector<Card>, 7> tableau;
    for (int i = 0; i < 7; ++i) tableau[i] = stackCards(localGs, tableauName(i));
    std::vector<Card> waste = stackCards(localGs, g_stackPrefix + "waste");
    std::vector<Card> talon = stackCards(localGs, g_stackPrefix + "talon");
    std::array<std::vector<Card>, 4> foundations;
    for (int i = 0; i < 4; ++i) foundations[i] = stackCards(localGs, foundationName(i));
    hydratePerfectInformation(localGs, tableau, talon, waste, foundations);
    int totalCards = (int)talon.size() + (int)waste.size();
    for (const auto& pile : tableau) totalCards += (int)pile.size();
    for (const auto& pile : foundations) totalCards += (int)pile.size();
    // The UI object exists briefly before the deal populates all stacks. Do
    // not poison the persistent planner/dead cache with that empty snapshot.
    if (totalCards != 52) return;
    rememberTableauPosition(tableau, now);
    const GameState expertState = makeGameState(tableau, foundations, talon, waste);
    logPerfectPlannerSnapshot(expertState);
    // A completed board briefly remains attached to the UI before the result
    // screen replaces LocalGs.  It has no legal moves by definition and must
    // not be reported/recovered as a strategic stall.
    if (virtualFoundationCount(expertState) == 52) return;
    // Observe the fully hydrated state before adding it to the generic recent
    // history. Strategic progress owns a separate epoch/visit model so normal
    // Draw-3 traversal is not mistaken for a repeated dead position.
    observeStrategicProgress(expertState, now);
    rememberSolverState(expertState, now);
    if (now - g_lastDiagMs > 2500) {
        g_lastDiagMs = now;
        SOL_LOG("state gs=%p tableau=[%d,%d,%d,%d,%d,%d,%d] waste=%d", localGs,
                (int)tableau[0].size(), (int)tableau[1].size(), (int)tableau[2].size(),
                (int)tableau[3].size(), (int)tableau[4].size(), (int)tableau[5].size(),
                (int)tableau[6].size(), (int)waste.size());
    }

    // One unified planner ranks foundation, tableau and stock actions together.
    // This is what allows it to skip a tempting legal move when a calculated
    // Talon branch unlocks a materially stronger continuation.
    if (chooseAndExecuteExpertMove(localGs, expertState)) {
        g_lastMoveMs = now;
        return;
    }

    // The search normally returns within one or two frames.  If its frontier
    // is exhausted, or three complete stock passes produced no progress, use
    // a narrow legal-progress escape hatch rather than leaving the bot idle.
    if (!g_waitingForMoveResult && g_persistentPlanner.frontier.empty() &&
        attemptEmergencyProgress(localGs, expertState)) {
        clearPlannerFrontier();
        g_lastMoveMs = now;
        return;
    }
    logStalledExpertState(expertState, now);
}
