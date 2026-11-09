#!/usr/bin/env bash
# Aqulify eltávolítása Linuxról (a mentett beállítások a ~/.config/aqulify mappában maradnak,
# a --purge kapcsolóval azok is törlődnek).
set -u

# >>> i18n (előállítva: tools/i18n_build.py – ne szerkeszd kézzel; források: i18n/scripts/<nyelv>.json)
# Nyelv: AQULIFY_LANG, a felületen választott nyelv (~/.config/aqulify/language.txt), majd a rendszeré
aq_detect_lang() {
    local l="${AQULIFY_LANG:-}" f="${XDG_CONFIG_HOME:-$HOME/.config}/aqulify/language.txt"
    [ -z "$l" ] && [ -r "$f" ] && l="$(head -n 1 "$f" 2>/dev/null || true)"
    [ -z "$l" ] && l="${LC_ALL:-${LC_MESSAGES:-${LANG:-en}}}"
    l="${l%%[_.@-]*}"
    case "$l" in
        hu|en|es|fr|zh|de|it|nl|pl|ru|pt|ro|el|tr|fi|da|nb|sv|he|ja|ko|cs|uk) printf "%s" "$l" ;;
        *) printf "en" ;;
    esac
}
AQ_LANG="$(aq_detect_lang)"
# msg_text NYELV KULCS – a szöveg (printf formátum) az adott nyelven; ha nincs, angolul
msg_text() {
    local f=""
    case "$1" in
    hu) case "$2" in
        common.errorPrefix) f='[Hiba]' ;;
        uninst.purged) f='Az Aqulify és a mentett beállításai eltávolítva.' ;;
        uninst.removed) f='Az Aqulify eltávolítva. (A beállítások megmaradtak: ~/.config/aqulify — törlés: ./uninstall.sh --purge)' ;;
        esac ;;
    es) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        uninst.purged) f='Aqulify y sus ajustes guardados han sido eliminados.' ;;
        uninst.removed) f='Aqulify ha sido eliminado. (Se han conservado los ajustes: ~/.config/aqulify — para borrarlos: ./uninstall.sh --purge)' ;;
        esac ;;
    fr) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        uninst.purged) f='Aqulify et ses paramètres enregistrés ont été supprimés.' ;;
        uninst.removed) f='Aqulify a été supprimé. (Les paramètres ont été conservés : ~/.config/aqulify — pour les supprimer : ./uninstall.sh --purge)' ;;
        esac ;;
    zh) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        uninst.purged) f='Aqulify 及其保存的设置已被清除。' ;;
        uninst.removed) f='Aqulify 已被移除。（保留了设置：~/.config/aqulify — 如需删除：./uninstall.sh --purge）' ;;
        esac ;;
    de) case "$2" in
        common.errorPrefix) f='[Fehler]' ;;
        uninst.purged) f='Aqulify und alle gespeicherten Einstellungen wurden entfernt.' ;;
        uninst.removed) f='Aqulify wurde entfernt. (Einstellungen beibehalten: ~/.config/aqulify – zum Löschen: ./uninstall.sh --purge)' ;;
        esac ;;
    it) case "$2" in
        common.errorPrefix) f='[Errore]' ;;
        uninst.purged) f='Aqulify e le relative impostazioni salvate sono stati rimossi.' ;;
        uninst.removed) f='Aqulify è stato rimosso. (Impostazioni conservate in: ~/.config/aqulify — per eliminarle: ./uninstall.sh --purge)' ;;
        esac ;;
    nl) case "$2" in
        common.errorPrefix) f='[Fout]' ;;
        uninst.purged) f='Aqulify en alle opgeslagen instellingen zijn verwijderd.' ;;
        uninst.removed) f='Aqulify is verwijderd. (Instellingen zijn behouden: ~/.config/aqulify — om te wissen: ./uninstall.sh --purge)' ;;
        esac ;;
    pl) case "$2" in
        common.errorPrefix) f='[Błąd]' ;;
        uninst.purged) f='Aqulify i zapisane ustawienia zostały usunięte.' ;;
        uninst.removed) f='Aqulify został usunięty. (Ustawienia zachowano w: ~/.config/aqulify — usunięcie: ./uninstall.sh --purge)' ;;
        esac ;;
    ru) case "$2" in
        common.errorPrefix) f='[Ошибка]' ;;
        uninst.purged) f='Aqulify и сохраненные настройки удалены.' ;;
        uninst.removed) f='Aqulify удален. (Настройки сохранены: ~/.config/aqulify — для удаления: ./uninstall.sh --purge)' ;;
        esac ;;
    pt) case "$2" in
        common.errorPrefix) f='[Erro]' ;;
        uninst.purged) f='O Aqulify e todas as configurações salvas foram removidos.' ;;
        uninst.removed) f='O Aqulify foi removido. (Configurações mantidas em: ~/.config/aqulify – para apagar: ./uninstall.sh --purge)' ;;
        esac ;;
    ro) case "$2" in
        common.errorPrefix) f='[Eroare]' ;;
        uninst.purged) f='Aqulify și setările salvate au fost eliminate.' ;;
        uninst.removed) f='Aqulify a fost eliminat. (Setări păstrate în: ~/.config/aqulify – pentru ștergere: ./uninstall.sh --purge)' ;;
        esac ;;
    el) case "$2" in
        common.errorPrefix) f='[Σφάλμα]' ;;
        uninst.purged) f='Το Aqulify και οι αποθηκευμένες ρυθμίσεις καταργήθηκαν.' ;;
        uninst.removed) f='Το Aqulify καταργήθηκε. (Οι ρυθμίσεις διατηρήθηκαν στο: ~/.config/aqulify – για διαγραφή: ./uninstall.sh --purge)' ;;
        esac ;;
    tr) case "$2" in
        common.errorPrefix) f='[Hata]' ;;
        uninst.purged) f='Aqulify ve kaydedilen ayarlar kaldırıldı.' ;;
        uninst.removed) f='Aqulify kaldırıldı. (Ayarlar korundu: ~/.config/aqulify – silmek için: ./uninstall.sh --purge)' ;;
        esac ;;
    fi) case "$2" in
        common.errorPrefix) f='[Virhe]' ;;
        uninst.purged) f='Aqulify ja tallennetut asetukset on poistettu.' ;;
        uninst.removed) f='Aqulify on poistettu. (Asetukset säilytetty: ~/.config/aqulify – poistaaksesi ne: ./uninstall.sh --purge)' ;;
        esac ;;
    da) case "$2" in
        common.errorPrefix) f='[Fejl]' ;;
        uninst.purged) f='Aqulify og alle gemte indstillinger er fjernet.' ;;
        uninst.removed) f='Aqulify er fjernet. (Indstillinger bevaret i: ~/.config/aqulify – slet med: ./uninstall.sh --purge)' ;;
        esac ;;
    nb) case "$2" in
        common.errorPrefix) f='[Feil]' ;;
        uninst.purged) f='Aqulify og alle lagrede innstillinger er fjernet.' ;;
        uninst.removed) f='Aqulify er fjernet. (Innstillinger beholdt i: ~/.config/aqulify – slett med: ./uninstall.sh --purge)' ;;
        esac ;;
    sv) case "$2" in
        common.errorPrefix) f='[Fel]' ;;
        uninst.purged) f='Aqulify och alla sparade inställningar har tagits bort.' ;;
        uninst.removed) f='Aqulify har tagits bort. (Inställningar finns kvar i: ~/.config/aqulify – ta bort med: ./uninstall.sh --purge)' ;;
        esac ;;
    he) case "$2" in
        common.errorPrefix) f='[שגיאה]' ;;
        uninst.purged) f='Aqulify וכל ההגדרות שנשמרו הוסרו.' ;;
        uninst.removed) f='Aqulify הוסר. (הגדרות נשמרו בנתיב: ~/.config/aqulify – למחיקה: ./uninstall.sh --purge)' ;;
        esac ;;
    ja) case "$2" in
        common.errorPrefix) f='[エラー]' ;;
        uninst.purged) f='Aqulifyおよび保存されたすべての設定が削除されました。' ;;
        uninst.removed) f='Aqulifyは削除されました。（設定は ~/.config/aqulify に保持されます。完全に削除するには: ./uninstall.sh --purge）' ;;
        esac ;;
    ko) case "$2" in
        common.errorPrefix) f='[오류]' ;;
        uninst.purged) f='Aqulify 및 저장된 모든 설정이 제거되었습니다.' ;;
        uninst.removed) f='Aqulify가 제거되었습니다. (설정은 ~/.config/aqulify 에 보존됨 – 삭제하려면: ./uninstall.sh --purge)' ;;
        esac ;;
    cs) case "$2" in
        common.errorPrefix) f='[Chyba]' ;;
        uninst.purged) f='Aqulify a veškerá uložená nastavení byla odstraněna.' ;;
        uninst.removed) f='Aqulify bylo odstraněno. (Nastavení zůstává v: ~/.config/aqulify – pro úplné smazání: ./uninstall.sh --purge)' ;;
        esac ;;
    uk) case "$2" in
        common.errorPrefix) f='[Помилка]' ;;
        uninst.purged) f='Aqulify та всі збережені налаштування видалено.' ;;
        uninst.removed) f='Aqulify видалено. (Налаштування збережено в: ~/.config/aqulify – видалити повністю: ./uninstall.sh --purge)' ;;
        esac ;;
    esac
    if [ -z "$f" ]; then
        case "$2" in
        common.errorPrefix) f='[Error]' ;;
        uninst.purged) f='Aqulify and its saved settings have been removed.' ;;
        uninst.removed) f='Aqulify has been removed. (The settings were kept: ~/.config/aqulify — to delete them: ./uninstall.sh --purge)' ;;
        esac
    fi
    printf "%s" "$f"
}
# msg KULCS [argumentumok...] – a választott nyelven (%s helyére az argumentumok)
msg() {
    local f
    f="$(msg_text "$AQ_LANG" "$1")"; shift
    # shellcheck disable=SC2059
    printf "$f" "$@"
}
# <<< i18n

PREFIX="${HOME}/.local"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/aqulify.service"

# Ha az Aqulify volt az alapértelmezett kimenet, a WirePlumber magától visszavált a hangkártyára
systemctl --user disable --now aqulify.service >/dev/null 2>&1 || true
rm -f "$UNIT"
systemctl --user daemon-reload >/dev/null 2>&1 || true

rm -f "$PREFIX/bin/aqulify-engine" "$PREFIX/bin/aqulify-gui" "$PREFIX/bin/aqulify-play"
rm -rf "$PREFIX/share/aqulify"
rm -f "$PREFIX/share/applications/io.github.szorgos.Aqulify.desktop" "$PREFIX/share/applications/aqulify.desktop"
rm -f "$PREFIX/share/icons/hicolor/256x256/apps/aqulify.png"
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true

if [ "${1:-}" = "--purge" ]; then
    rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/aqulify"
    msg uninst.purged; echo
else
    msg uninst.removed; echo
fi
