#include <nfd.h>

extern "C" {
nfdresult_t NFD_Init(void) { return NFD_OKAY; }
void NFD_Quit(void) {}
nfdresult_t NFD_OpenDialogN(nfdchar_t**, const nfdfilteritem_t*, nfdfiltersize_t, const nfdchar_t*) { return NFD_CANCEL; }
nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t**, const nfdfilteritem_t*, nfdfiltersize_t, const nfdchar_t*) { return NFD_CANCEL; }
nfdresult_t NFD_SaveDialogN(nfdchar_t**, const nfdfilteritem_t*, nfdfiltersize_t, const nfdchar_t*, const nfdchar_t*) { return NFD_CANCEL; }
nfdresult_t NFD_PickFolderN(nfdchar_t**, const nfdchar_t*) { return NFD_CANCEL; }
const nfdchar_t* NFD_GetError(void) { return "Android file dialogs are handled by Java"; }
void NFD_FreePathN(nfdchar_t*) {}
void NFD_PathSet_Free(const nfdpathset_t*) {}
}