#include "gameObjects.h"

GameObjects::GameObjects(GraphicsEngine *graphicsEnginePtr) : graphicsEnginePtr(graphicsEnginePtr) {}

void GameObjects::addRect(float x, float y, float width, float height, std::string texture, bool isStatic) {
	newRects.push_back(Rect(x, y, width, height, texture, isStatic));
}

void GameObjects::checkNewRects() {
	if (newRects.size() > 0) {
		graphicsEnginePtr->addRects(newRects);
		rects.insert(rects.end(), newRects.begin(), newRects.end());
		newRects.clear();
	}
}