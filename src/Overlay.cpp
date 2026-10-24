// On-screen label, predicted-path drawing, and the pause-menu settings button.

#include "State.hpp"

#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

namespace {

cocos2d::CCLabelBMFont* g_label = nullptr;
cocos2d::CCDrawNode* g_path = nullptr;

// Both nodes are retained by us, so the pointers stay valid after the level is
// torn down; a node whose parent is gone belongs to a level that has ended.
bool alive(cocos2d::CCNode* node) {
    auto pl = PlayLayer::get();
    return node && pl && node->getParent() && (node->getParent() == pl || node->getParent() == pl->m_objectLayer);
}

template <class T>
void releaseNode(T*& node) {
    if (!node) return;
    node->removeFromParent();
    node->release();
    node = nullptr;
}

}

namespace assist {

void overlayRefresh() {
    if (!alive(g_label)) return;
    auto const& c = cfg();
    bool show = c.enabled && c.showLabel;
    g_label->setVisible(show);
    if (!show) return;

    auto const& s = stats();
    std::string text = fmt::format("Bot-assisted ({} learned spots)\nthis session: learned {}  blocked {}  saved {}  delayed {}  buffered {}",
                                   learnWindowCount(), s.learned, s.blocked, s.saved, s.delayed, s.buffered);
    if (!s.last.empty()) text += "\nlast: " + s.last;
    g_label->setString(text.c_str());
}

void overlayShowPath(std::vector<cocos2d::CCPoint> const& path, bool died) {
    if (!alive(g_path) || !cfg().showPath) return;
    g_path->clear();
    auto color = died ? cocos2d::ccc4f(1.f, 0.3f, 0.3f, 0.9f) : cocos2d::ccc4f(0.3f, 1.f, 0.4f, 0.9f);
    for (size_t k = 1; k < path.size(); k++) {
        g_path->drawSegment(path[k - 1], path[k], 0.8f, color);
    }
}

void overlayClearPath() {
    if (alive(g_path)) g_path->clear();
}

}

class $modify(OverlayPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        releaseNode(g_label);
        releaseNode(g_path);

        auto win = cocos2d::CCDirector::get()->getWinSize();
        g_label = cocos2d::CCLabelBMFont::create("Bot-assisted", "chatFont.fnt");
        if (g_label) {
            g_label->retain();
            g_label->setID("assist-label"_spr);
            g_label->setAnchorPoint({0.f, 1.f});
            g_label->setAlignment(cocos2d::kCCTextAlignmentLeft);
            g_label->setScale(0.5f);
            g_label->setOpacity(170);
            g_label->setPosition({5.f, win.height - 5.f});
            this->addChild(g_label, 1000);
        }

        g_path = cocos2d::CCDrawNode::create();
        if (g_path) {
            g_path->retain();
            g_path->setID("assist-path"_spr);
            m_objectLayer->addChild(g_path, 500);
        }

        assist::stats() = assist::Stats();
        assist::overlayRefresh();
    }

    void onQuit() {
        releaseNode(g_label);
        releaseNode(g_path);
        PlayLayer::onQuit();
    }
};

class $modify(AssistPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = cocos2d::CCMenu::create();
        auto spr = ButtonSprite::create("Assist");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AssistPause::onAssistSettings));
        menu->addChild(btn);

        auto win = cocos2d::CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 60.f, 60.f});
        menu->setID("assist-menu"_spr);
        this->addChild(menu);
    }

    void onAssistSettings(cocos2d::CCObject*) {
        openSettingsPopup(Mod::get(), false);
    }
};
