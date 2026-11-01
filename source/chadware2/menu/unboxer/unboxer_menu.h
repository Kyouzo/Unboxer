#pragma once

// Cases: pick a case, put it (and keys) in your inventory, choose what the next opening gives you.
void RenderCasesPage();

// Add items: any item the game has (skins, knives, gloves, agents, stickers, charms, music kits, pins, pets) into
// your inventory, with float / pattern / StatTrak / name tag / charm.
void RenderAddItemsPage();
// Every frame (menu): false while the Add items tab isn't on screen - its 3D preview is closed then.
void KeepAddItemsPreview(bool onScreen);

// My Items: everything Unboxer put in your inventory, to remove.
void RenderMyItemsPage();

// Upgrader: put one of your skins in for a better one - win it or lose yours on the wheel.
void RenderUpgraderPage();

// Every frame (render thread, menu open or not): Lootline's prices for CS2's own screens (Unboxer::SetGamePrices),
// worked out every 2 s, handed over when something changed.
void PublishGamePrices();
