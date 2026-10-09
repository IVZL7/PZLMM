# Project Zomboid Local Mod Manager for Windows
Project Zomboid Local Mod Manager for managing installations in the User mods folder using Workshop installed mods
**AI vibe coded** project for my own satisfaction (saves the pain of re-checking what mods I have enabled out of hundreds more to send to my broke friends who cannot afford this masterpiece of a game) using wxWidgets as UI library with sorting, filtering, background activity reporting, ZIP archiving and progress indicators.

## What's included
**User mods** — scans C:\Users\<Username>\Zomboid\mods, displays mod metadata and posters, and shows each mod’s position in default.txt.
**Steam Workshop** — scans 108600\<WorkshopID>\mods\<ModFolder> and groups all game-version directories belonging to the same mod into one row. The common directory is treated as shared assets, not a separate version.
**Copy / Update** — copies Workshop mods directly into the user mods folder, including common, every version directory, and other assets.
**ZIP archiving** — rate limited archiving of mods, available in batch, both from User and Workshop folders.

**Click a column header:** sort by that field; clicking again reverses the order.
**Right-click a column header:** open a context menu with showing mod folder in the explorer, substring filter, clear-filter option, and optionally case-sensitive matching.
**Multiple filters:** filter by mod name, ID, author, version, Workshop ID, and enabled status simultaneously.
