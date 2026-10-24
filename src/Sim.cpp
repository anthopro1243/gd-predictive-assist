// Look-ahead simulation.
//
// A hidden "fake" PlayerObject is created per level. To test an input, the
// real player's physics state is copied onto the fake, and the fake is stepped
// with the game's own collision and movement code. The real player and the
// level are never stepped, so nothing has to be restored afterwards.
//
// While a simulation runs, every hook below blocks side effects: deaths,
// triggers, sounds, trail points, jump counters, and object activation. Objects
// the simulation cannot model (orbs, pads, most portals) end the simulation as
// "unknown", which callers treat as safe, so the assist falls back to vanilla
// behaviour instead of guessing.
//
// Same idea as the trajectory preview in bot mods; this is an independent
// implementation.

#include "State.hpp"

#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <unordered_set>

namespace {

PlayerObject* g_fake = nullptr;
bool g_active = false;
bool g_died = false;
bool g_unknown = false;
float g_delta = 0.25f;  // last dt the game passed to PlayerObject::update
std::unordered_set<GameObject*> g_applied;

// Portals the simulation applies itself: speed and size. Everything else that
// can be activated ends the simulation.
constexpr int ID_SPEED_SLOW = 200;
constexpr int ID_SPEED_NORMAL = 201;
constexpr int ID_SPEED_FAST = 202;
constexpr int ID_SPEED_FASTER = 203;
constexpr int ID_SPEED_FASTEST = 1334;
constexpr int ID_SIZE_MINI = 101;
constexpr int ID_SIZE_NORMAL = 99;

bool isHandledPortalId(int id) {
    switch (id) {
        case ID_SPEED_SLOW: case ID_SPEED_NORMAL: case ID_SPEED_FAST:
        case ID_SPEED_FASTER: case ID_SPEED_FASTEST:
        case ID_SIZE_MINI: case ID_SIZE_NORMAL:
            return true;
        default:
            return false;
    }
}

// Object types the simulation keeps active in collision checks. Physical
// objects collide normally; interactive ones stay so that touching them can be
// detected and end the simulation as "unknown".
bool keepForSim(GameObject* obj) {
    if (isHandledPortalId(obj->m_objectID)) return true;
    switch (obj->m_objectType) {
        // physical
        case GameObjectType::Solid:
        case GameObjectType::Hazard:
        case GameObjectType::Slope:
        case GameObjectType::AnimatedHazard:
        // interactive: detected, not modelled
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            // decorations, triggers, coins, collectibles, breakables, special
            return false;
    }
}

bool overlaps(PlayerObject* p, GameObject* obj) {
    cocos2d::CCRect objRect = obj->getObjectRect();
    cocos2d::CCRect playerRect = p->GameObject::getObjectRect();
    return objRect.intersectsRect(playerRect);
}

// Called when the simulated player could activate an object.
void simInteract(PlayerObject* p, GameObject* obj) {
    if (!obj || g_applied.contains(obj) || !overlaps(p, obj)) return;
    g_applied.insert(obj);

    switch (obj->m_objectID) {
        case ID_SPEED_SLOW: p->m_playerSpeed = 0.7f; return;
        case ID_SPEED_NORMAL: p->m_playerSpeed = 0.9f; return;
        case ID_SPEED_FAST: p->m_playerSpeed = 1.1f; return;
        case ID_SPEED_FASTER: p->m_playerSpeed = 1.3f; return;
        case ID_SPEED_FASTEST: p->m_playerSpeed = 1.6f; return;
        case ID_SIZE_MINI:
            p->togglePlayerScale(true, true);
            p->updatePlayerScale();
            return;
        case ID_SIZE_NORMAL:
            p->togglePlayerScale(false, true);
            p->updatePlayerScale();
            return;
        default:
            g_unknown = true;
            return;
    }
}

void clearCollisionLogs(PlayerObject* p) {
    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
}

// Copies the physics state of `src` onto `dst` by direct assignment. Mode
// flags are assigned rather than toggled, because the toggle functions also
// change the level (ground, camera) for the real game.
void copyPlayer(PlayerObject* src, PlayerObject* dst) {
#define CP(field) dst->field = src->field
    dst->setPosition(src->getPosition());
    dst->setRotation(src->getRotation());
    CP(m_position);

    // gamemode and orientation
    CP(m_isShip); CP(m_isBird); CP(m_isBall); CP(m_isDart);
    CP(m_isRobot); CP(m_isSpider); CP(m_isSwing);
    CP(m_isUpsideDown); CP(m_isSideways); CP(m_isGoingLeft);
    CP(m_vehicleSize); CP(m_playerSpeed); CP(m_isSecondPlayer);
    CP(m_isPlatformer);

    // vertical motion
    CP(m_yVelocity); CP(m_fallSpeed); CP(m_yVelocityBeforeSlope);
    CP(m_yVelocityRelated); CP(m_yVelocityRelated3); CP(m_groundYVelocity);
    CP(m_speedMultiplier); CP(m_yStart); CP(m_gravity); CP(m_gravityMod);
    CP(m_accelerationOrSpeed); CP(m_isAccelerating); CP(m_maybeIsFalling);
    CP(m_fallStartY);

    // ground and collision state
    CP(m_isOnGround); CP(m_isOnGround2); CP(m_isOnGround3); CP(m_isOnGround4);
    CP(m_lastGroundObject); CP(m_maybeLastGroundObject); CP(m_preLastGroundObject);
    CP(m_collidedObject); CP(m_collidingWithLeft); CP(m_collidingWithRight);
    CP(m_lastCollisionBottom); CP(m_lastCollisionTop);
    CP(m_lastCollisionLeft); CP(m_lastCollisionRight);
    CP(m_collidedTopMinY); CP(m_collidedBottomMaxY);
    CP(m_collidedLeftMaxX); CP(m_collidedRightMinX);
    CP(m_objectSnappedTo); CP(m_snapDistance);
    CP(m_lastLandTime); CP(m_lastGroundedPos); CP(m_groundObjectMaterial);
    CP(m_wasTeleported); CP(m_fixGravityBug); CP(m_reverseSync);

    // slopes
    CP(m_isOnSlope); CP(m_wasOnSlope); CP(m_slopeVelocity); CP(m_slopeAngle);
    CP(m_slopeAngleRadians); CP(m_slopeRotation); CP(m_slopeStartTime);
    CP(m_slopeEndTime); CP(m_currentSlope); CP(m_currentSlope2);
    CP(m_currentPotentialSlope); CP(m_currentSlopeYVelocity);
    CP(m_isCollidingWithSlope); CP(m_collidingWithSlopeId);
    CP(m_slopeFlipGravityRelated); CP(m_slopeSlidingMaybeRotated);
    CP(m_maybeUpsideDownSlope); CP(m_isCurrentSlopeTop);
    CP(m_potentialSlopeMap);

    // rotation
    CP(m_rotationSpeed); CP(m_rotateSpeed); CP(m_isRotating);
    CP(m_isBallRotating); CP(m_isBallRotating2); CP(m_shipRotation);
    CP(m_rotateObjectsRelated);

    // jump, ring and pad state
    CP(m_jumpBuffered); CP(m_wasJumpBuffered); CP(m_stateJumpBuffered);
    CP(m_wasRobotJump); CP(m_fixRobotJump); CP(m_stateRingJump); CP(m_stateRingJump2);
    CP(m_ringJumpRelated); CP(m_ringRelatedSet); CP(m_touchedRings);
    CP(m_touchedRing); CP(m_touchedCustomRing); CP(m_touchedGravityPortal);
    CP(m_touchedPad); CP(m_padRingRelated); CP(m_jumpPadRelated);
    CP(m_maybeTouchedBreakableBlock); CP(m_blackOrbRelated);
    CP(m_hasEverJumped); CP(m_hasEverHitRing);
    CP(m_lastJumpTime); CP(m_lastFlipTime); CP(m_lastSpiderFlipTime);
    CP(m_lastActivatedPortal); CP(m_lastPortalPos); CP(m_gameModeChangedTime);

    // dash orbs
    CP(m_isDashing); CP(m_dashX); CP(m_dashY); CP(m_dashAngle);
    CP(m_dashStartTime); CP(m_dashRing);

    // input
    CP(m_holdingButtons); CP(m_holdingLeft); CP(m_holdingRight);
    CP(m_leftPressedFirst); CP(m_inputsLocked); CP(m_isLocked); CP(m_controlsDisabled);

    // platformer / horizontal (copied for completeness; platformer is skipped)
    CP(m_platformerXVelocity); CP(m_platformerVelocityRelated);
    CP(m_xVelocityRelated); CP(m_xVelocityRelated2);
    CP(m_reverseRelated); CP(m_maybeReverseSpeed); CP(m_maybeReverseAcceleration);
    CP(m_maybeIsBoosted); CP(m_decreaseBoostSlide); CP(m_scaleXRelatedTime);
    CP(m_scaleXRelated); CP(m_scaleXRelated2); CP(m_scaleXRelated3);
    CP(m_scaleXRelated4); CP(m_scaleXRelated5);

    // state flags used by the 2.2 physics
    CP(m_stateOnGround); CP(m_stateUnk); CP(m_stateNoStickX); CP(m_stateNoStickY);
    CP(m_stateUnk2); CP(m_stateBoostX); CP(m_stateBoostY); CP(m_maybeStateForce2);
    CP(m_stateScale); CP(m_stateNoAutoJump); CP(m_stateDartSlide);
    CP(m_stateHitHead); CP(m_stateFlipGravity); CP(m_stateForce);
    CP(m_stateForceVector); CP(m_affectedByForces);

    // misc
    CP(m_totalTime); CP(m_followRelated); CP(m_playerFollowFloats);
    CP(m_unk3d0); CP(m_unk3e0); CP(m_unk3e1); CP(m_unk669); CP(m_unkBool5);
    CP(m_enable22Changes); CP(m_enableImpulseFix); CP(m_ignoreDamage);
    CP(m_physDeltaRelated); CP(m_lastMovedTime); CP(m_playerSpeedAC);
#undef CP

    // The simulation must always be able to die, even if something set the
    // real player to ignore damage.
    dst->m_ignoreDamage = false;
    dst->m_isDead = false;
    dst->updatePlayerScale();
}

}

namespace assist {

bool simActive() { return g_active; }

bool simReady(PlayLayer* pl) {
    return pl && g_fake && g_fake->getParent() == pl->m_objectLayer;
}

SimResult simulate(PlayLayer* pl, PlayerObject* real, std::vector<SimEvent> const& events, int totalTicks, bool recordPath) {
    SimResult r;
    if (!simReady(pl) || !real || g_active) {
        r.unknown = true;
        return r;
    }

    copyPlayer(real, g_fake);
    g_active = true;
    g_died = false;
    g_unknown = false;
    g_applied.clear();

    float dt = g_delta;
    for (int t = 0; t < totalTicks; t++) {
        if (recordPath) r.path.push_back(g_fake->getPosition());

        clearCollisionLogs(g_fake);
        pl->checkCollisions(g_fake, dt, false);
        if (g_died || g_unknown) break;

        for (auto const& e : events) {
            if (e.tick != t) continue;
            if (e.down) g_fake->pushButton(PlayerButton::Jump);
            else g_fake->releaseButton(PlayerButton::Jump);
        }

        g_fake->update(dt);
        g_fake->updateRotation(dt);
        g_fake->updatePlayerScale();
        r.ticks = t + 1;
    }

    // Collision at the final position.
    if (!g_died && !g_unknown) {
        clearCollisionLogs(g_fake);
        pl->checkCollisions(g_fake, dt, false);
        if (recordPath) r.path.push_back(g_fake->getPosition());
    }

    r.died = g_died;
    r.unknown = g_unknown && !g_died;
    g_active = false;
    g_applied.clear();
    return r;
}

}

// ---- hooks -----------------------------------------------------------------

class $modify(SimPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        if (g_fake) {
            g_fake->removeFromParent();
            g_fake->release();
            g_fake = nullptr;
        }
        g_fake = PlayerObject::create(1, 1, this, this, true);
        if (g_fake) {
            g_fake->retain();
            g_fake->setVisible(false);
            g_fake->setPosition({0.f, 105.f});
            m_objectLayer->addChild(g_fake);
        }
    }

    void onQuit() {
        if (g_fake) {
            g_fake->removeFromParent();
            g_fake->release();
            g_fake = nullptr;
        }
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_active || (player && player == g_fake)) {
            // The anti-cheat spike is a hidden hazard the real game ignores
            // unless it has flagged a cheat; it is not a real death.
            if (object && object == m_anticheatSpike) return;
            g_died = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        assist::bumpResetEpoch();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        assist::bumpResetEpoch();
    }

    void loadFromCheckpoint(CheckpointObject* object) {
        PlayLayer::loadFromCheckpoint(object);
        assist::bumpResetEpoch();
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (g_active) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void levelComplete() {
        if (g_active) return;
        PlayLayer::levelComplete();
    }
};

class $modify(SimGameLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!g_active || player != g_fake || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        std::vector<GameObject*> disabled;
        size_t n = std::min(static_cast<size_t>(std::max(objectCount, 0)), objects->size());
        for (size_t i = 0; i < n; i++) {
            auto obj = (*objects)[i];
            if (!obj || keepForSim(obj)) continue;
            if (obj->m_isDisabled || obj->m_isDisabled2) continue;
            obj->m_isDisabled = true;
            obj->m_isDisabled2 = true;
            disabled.push_back(obj);
        }

        GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);

        for (auto obj : disabled) {
            obj->m_isDisabled = false;
            obj->m_isDisabled2 = false;
        }
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_active) {
            simInteract(player, object);
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (g_active) {
            g_unknown = true;
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (g_active) {
            simInteract(player, object);
            return;
        }
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
        if (g_active) {
            g_unknown = true;
            return;
        }
        GJBaseGameLayer::bumpPlayer(player, object);
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (g_active) return;
        GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (g_active) return;
        GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (g_active) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
};

class $modify(SimPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!g_active && this != g_fake) g_delta = dt;
        PlayerObject::update(dt);
    }

    void ringJump(RingObject* object, bool skipCheck) {
        if (g_active) {
            g_unknown = true;
            return;
        }
        PlayerObject::ringJump(object, skipCheck);
    }

    void incrementJumps() {
        if (g_active) return;
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (g_active) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(SimHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (g_active) return;
        HardStreak::addPoint(point);
    }
};

class $modify(SimGameObject, GameObject) {
    void playShineEffect() {
        if (g_active) return;
        GameObject::playShineEffect();
    }
};

class $modify(SimEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (g_active) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};
