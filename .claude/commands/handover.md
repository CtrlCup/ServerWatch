---
description: Übergibt ALLE aktiven Agenten dieses Projekts an einen anderen Rechner (commit, push, PR, Handoff-Notiz, automatischer Server-Trigger)
---

Führe jetzt eine Übergabe dieses Projekts an einen anderen Rechner durch.
Wichtig: `vibe handover` wirkt auf **alle** gerade aktiven Agenten-Sessions
in diesem Projekt, nicht nur auf deine eigene — falls parallel andere
Agenten an anderen Aufgaben im selben Projekt arbeiten, werden deren Branches
automatisch mit übergeben und committet/gepusht.

1. Committe deine eigenen offenen Änderungen selbst mit einer
   aussagekräftigen Commit-Message (nicht dem Script überlassen — du kennst
   den Kontext). Andere aktive Agenten-Worktrees committet das Script
   selbst mit einer generischen Zwischenstand-Message.
2. Formuliere eine kurze, allgemeine Notiz für den Rechnerwechsel (gilt für
   alle mit übergebenen Branches, nicht nur deinen eigenen — z. B. "Rechner
   gewechselt, alle Agenten pausiert" statt einer aufgabenspezifischen
   Notiz).
3. Führe `vibe handover "<Notiz aus Schritt 2>"` aus. Das Script erledigt
   automatisch für jeden aktiven Branch im Projekt: Push, Pull-Request
   anlegen/aktualisieren (falls `gh` verfügbar), Notiz an dessen
   `.vibe/handoff.md` anhängen — und, falls du gerade nicht auf dem Server
   bist, automatisch die Übergabe an den Server per SSH auslösen (inkl.
   Erst-Klonen des Projekts dort, falls nötig). Auf der Zielmaschine werden
   alle übergebenen Sessions automatisch wieder geöffnet.
4. Gib die Ausgabe des Scripts an den Nutzer weiter, insbesondere ob die
   Übergabe automatisch an den Server ging oder der Nutzer sie manuell auf
   einem anderen Rechner mit `vibe pull <branch>` fortsetzen muss.

Halte dich dabei an die Regeln aus AGENTS.md/CLAUDE.md.
