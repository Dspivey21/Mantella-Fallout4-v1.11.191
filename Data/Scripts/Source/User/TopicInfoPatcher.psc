Scriptname TopicInfoPatcher extends ScriptObject Native

; Native functions take int form IDs — form pointer types crash on AE 1.11.191
int Function PatchTopicInfoNative(int formID, string text) Native Global
Function SetOverrideFileNameNative(int formID, string name) Native Global

; Papyrus wrappers — accept Topic form, convert to int form ID for the native
int Function PatchTopicInfo(Topic tpTopic, string text) Global
    return PatchTopicInfoNative(tpTopic.GetFormID(), text)
EndFunction

Function SetOverrideFileName(Topic TPInfo, string name) Global
    SetOverrideFileNameNative(TPInfo.GetFormID(), name)
EndFunction

Function ClearCache() Native Global

string Function StringRemoveWhiteSpace(string text) Native Global

; filetype = 3 PNG
; filetype = 2 TGA

Function TakeScreenShot(string filename, int filetype, int sstype = 1) Native Global
bool Function isMenuModeActive() Native Global

; Functions to save and restore GameSettings

bool Function saveFloat(string key) Native Global
bool Function saveInt(string key) Native Global
bool Function restoreFloat(string key) Native Global
bool Function restoreInt(string key) Native Global

; returning Actor values from plugins is problematic at this time
; Either some Actors return as none, or will cause the game to CTD.
; As a workaround we just return the coords for the crosshair actor
; And then we just use Game.FindClosestActor() to get the Actor

Float Function GetLastActorCoordX() Native Global
Float Function GetLastActorCoordY() Native Global
Float Function GetLastActorCoordZ() Native Global

Float[] Function GetLastActorCoords() Global
    Float[] coords = new Float[3]
    coords[0] = GetLastActorCoordX()
    coords[1] = GetLastActorCoordY()
    coords[2] = GetLastActorCoordZ()
    return coords
EndFunction
