#!/usr/bin/env bash
# ==============================================================================
# Aqulify – telepítő Linuxra (PipeWire)
#
# Mit csinál:
#   1. telepíti a fordításhoz szükséges csomagokat (sudo jelszót kérhet),
#   2. lefordítja a motort (és ha lehet, a saját ablakos felületet),
#   3. felmásolja a felhasználó saját mappájába (~/.local), root jog nélkül,
#   4. beállítja, hogy a motor bejelentkezéskor automatikusan induljon (systemd felhasználói szolgáltatás),
#   5. menübejegyzést készít ("Aqulify"),
#   6. kérésre az "Aqulify" eszközt teszi meg alapértelmezett hangkimenetnek.
#
# Eltávolítás: ./uninstall.sh
# Támogatott: Ubuntu / Debian / Mint / Pop!_OS, Fedora, Arch / Manjaro / EndeavourOS, openSUSE
# ==============================================================================
set -euo pipefail

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
        inst.title) f='Aqulify – telepítés Linuxra' ;;
        inst.noRoot) f='Ne rootként futtasd! A saját felhasználóddal indítsd (a csomagokhoz sudo jelszót fog kérni).' ;;
        inst.step1) f='[1/6] Szükséges csomagok telepítése...' ;;
        inst.noOsRelease) f='Nem állapítható meg a disztribúció (/etc/os-release).' ;;
        inst.unknownDistro1) f='Ismeretlen disztribúció (%s). Kell: C/C++20 fordító, cmake, pkg-config, libpipewire-0.3 és fftw3 (float) fejlesztői csomag;' ;;
        inst.unknownDistro2) f='a saját ablakhoz: gtk3 és webkit2gtk-4.1 (vagy 4.0) fejlesztői csomag.' ;;
        inst.noWebkit) f='A WebKitGTK csomag nem települt: a felület böngészőben fog megnyílni.' ;;
        inst.noPwCli) f='A pw-cli nem található. Biztosan PipeWire a hangrendszered?' ;;
        inst.systemPkg) f='Csomagból telepített Aqulify is van a gépen (/usr/bin). Ez a telepítés (~/.local) mostantól elsőbbséget kap; ha a csomagost használnád, utána futtasd az ./uninstall.sh-t.' ;;
        inst.step2) f='[2/6] Fordítás...' ;;
        inst.rnnSystem) f='RNNoise (beszéd zajcsökkentés): a rendszer csomagja.' ;;
        inst.rnnDownload) f='RNNoise modell letöltése (Ubuntu archívum, kb. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (beszéd zajcsökkentés): beépítve.' ;;
        inst.rnnFail) f='Az RNNoise modell nem tölthető le: a beszéd zajcsökkentése a saját, sávonkénti módszerrel működik.' ;;
        inst.engineFail) f='A motor fordítása sikertelen.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Saját ablakos felület: build/aqulify-gui' ;;
        inst.guiFail1) f='A saját ablakos (WebKitGTK) felület nem fordult le (részletek: build/gui-build.log).' ;;
        inst.guiFail2) f='Tartalék: Chromium alapú böngésző alkalmazásablaka, ha nincs ilyen, az alapértelmezett böngésző.' ;;
        inst.guiFail3) f='Javítás: telepítsd a gtk3 és webkit2gtk-4.1 fejlesztői csomagot, majd futtasd újra az ./install.sh-t.' ;;
        inst.step3) f='[3/6] Fájlok másolása (%s)...' ;;
        inst.legacy) f='Korábbi AetherAudio telepítés eltávolítása (a beállítások átkerülnek)...' ;;
        inst.filesOk) f='Fájlok a helyükön.' ;;
        inst.step4) f='[4/6] Automatikus indítás beállítása (systemd felhasználói szolgáltatás)...' ;;
        inst.unitDesc) f='Aqulify DSP motor (virtuális "Aqulify" hangkimenet)' ;;
        inst.serviceOk) f='A motor fut, és bejelentkezéskor magától elindul.' ;;
        inst.serviceFail) f='A systemd felhasználói szolgáltatás nem érhető el. Indítsd kézzel: %s &' ;;
        inst.step5) f='[5/6] Menübejegyzés...' ;;
        inst.desktopGeneric) f='Hangkorrekció és akusztikai vízesés' ;;
        inst.desktopComment) f='Parametrikus EQ, konvolver, crossfeed és true peak limiter a PipeWire-hez, élő 3D vízesés-analizátorral' ;;
        inst.menuOk) f='A menüben és az asztalon: Aqulify' ;;
        inst.step6) f='[6/6] Alapértelmezett hangkimenet' ;;
        inst.defaultExplain) f='Az alkalmazások hangja akkor megy át az Aqulify-n, ha az "Aqulify" a kimeneti eszköz.' ;;
        inst.legacyDefault) f='A korábbi AetherAudio volt az alapértelmezett kimenet: az Aqulify veszi át.' ;;
        inst.askDefault) f='Beállítsam most alapértelmezettnek? [i/N] ' ;;
        inst.defaultOk) f='Az "Aqulify" lett az alapértelmezett kimenet.' ;;
        inst.defaultFail) f='Nem sikerült; állítsd be a Hangbeállításokban (Kimenet: Aqulify).' ;;
        inst.defaultLater) f='Később a Hangbeállításokban választhatod ki (Kimenet: Aqulify).' ;;
        inst.done) f='Kész! Az Aqulify telepítve.' ;;
        inst.sumGui) f='Felület:        menü → Aqulify  (vagy: aqulify-gui)' ;;
        inst.sumState) f='Motor állapota: systemctl --user status aqulify' ;;
        inst.sumLog) f='Napló:          journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Eltávolítás:    ./uninstall.sh' ;;
        esac ;;
    es) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        inst.title) f='Aqulify – instalación en Linux' ;;
        inst.noRoot) f='¡No lo ejecutes como root! Inícialo con tu propio usuario (te pedirá la contraseña de sudo para los paquetes).' ;;
        inst.step1) f='[1/6] Instalando paquetes necesarios...' ;;
        inst.noOsRelease) f='No se puede determinar la distribución (/etc/os-release).' ;;
        inst.unknownDistro1) f='Distribución desconocida (%s). Necesario: compilador C/C++20, cmake, pkg-config, paquetes de desarrollo de libpipewire-0.3 y fftw3 (float);' ;;
        inst.unknownDistro2) f='para la ventana de la aplicación: paquetes de desarrollo gtk3 y webkit2gtk-4.1 (o 4.0).' ;;
        inst.noWebkit) f='El paquete WebKitGTK no está instalado: la interfaz se abrirá en un navegador.' ;;
        inst.noPwCli) f='No se encontró pw-cli. ¿Tu sistema de sonido es realmente PipeWire?' ;;
        inst.systemPkg) f='También hay un paquete de Aqulify instalado (/usr/bin). Esta instalación (~/.local) tendrá prioridad a partir de ahora; para usar la empaquetada, ejecuta ./uninstall.sh después.' ;;
        inst.step2) f='[2/6] Compilando...' ;;
        inst.rnnSystem) f='RNNoise (reducción de ruido de voz): paquete del sistema.' ;;
        inst.rnnDownload) f='Descargando el modelo RNNoise (archivo de Ubuntu, unos 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (reducción de ruido de voz): integrado.' ;;
        inst.rnnFail) f='No se pudo descargar el modelo RNNoise: la reducción de ruido de voz usará el método por bandas integrado.' ;;
        inst.engineFail) f='Falló la compilación del motor.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Interfaz de ventana: build/aqulify-gui' ;;
        inst.guiFail1) f='La interfaz de ventana (WebKitGTK) no se compiló (detalles: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativa: la ventana de aplicación de un navegador basado en Chromium o, si no hay ninguno, el navegador predeterminado.' ;;
        inst.guiFail3) f='Solución: instala los paquetes de desarrollo gtk3 y webkit2gtk-4.1, y ejecuta ./install.sh de nuevo.' ;;
        inst.step3) f='[3/6] Copiando archivos (%s)...' ;;
        inst.legacy) f='Eliminando la instalación anterior de AetherAudio (se conservan los ajustes)...' ;;
        inst.filesOk) f='Archivos en su lugar.' ;;
        inst.step4) f='[4/6] Configurando el inicio automático (servicio de usuario de systemd)...' ;;
        inst.unitDesc) f='Motor DSP de Aqulify (salida de audio virtual «Aqulify»)' ;;
        inst.serviceOk) f='El motor está en ejecución y se inicia automáticamente al iniciar sesión.' ;;
        inst.serviceFail) f='El servicio de usuario de systemd no está disponible. Inícialo manualmente: %s &' ;;
        inst.step5) f='[5/6] Entrada en el menú...' ;;
        inst.desktopGeneric) f='Ecualizador y analizador acústico en cascada' ;;
        inst.desktopComment) f='EQ paramétrico, convolucionador, crossfeed y limitador True Peak para PipeWire con analizador en cascada 3D en vivo' ;;
        inst.menuOk) f='En el menú y en el escritorio: Aqulify' ;;
        inst.step6) f='[6/6] Salida de audio predeterminada' ;;
        inst.defaultExplain) f='El audio de las aplicaciones pasa por Aqulify cuando «Aqulify» es el dispositivo de salida.' ;;
        inst.legacyDefault) f='La versión anterior AetherAudio era la salida predeterminada: Aqulify toma el relevo.' ;;
        inst.askDefault) f='¿Establecerla como predeterminada ahora? [y/N] ' ;;
        inst.defaultOk) f='«Aqulify» es ahora la salida predeterminada.' ;;
        inst.defaultFail) f='No ha funcionado; configúralo en los ajustes de Sonido (Salida: Aqulify).' ;;
        inst.defaultLater) f='Puedes seleccionarlo más tarde en los ajustes de Sonido (Salida: Aqulify).' ;;
        inst.done) f='¡Listo! Aqulify está instalado.' ;;
        inst.sumGui) f='Interfaz:       menú → Aqulify  (o: aqulify-gui)' ;;
        inst.sumState) f='Estado motor:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Registro:       journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Desinstalar:    ./uninstall.sh' ;;
        esac ;;
    fr) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        inst.title) f='Aqulify – installation sous Linux' ;;
        inst.noRoot) f='Ne l'"'"'exécutez pas en tant que root ! Lancez-le avec votre propre utilisateur (le mot de passe sudo sera demandé pour les paquets).' ;;
        inst.step1) f='[1/6] Installation des paquets requis...' ;;
        inst.noOsRelease) f='Impossible de déterminer la distribution (/etc/os-release).' ;;
        inst.unknownDistro1) f='Distribution inconnue (%s). Requis : compilateur C/C++20, cmake, pkg-config, paquets de développement libpipewire-0.3 et fftw3 (float) ;' ;;
        inst.unknownDistro2) f='pour la fenêtre de l'"'"'application : paquets de développement gtk3 et webkit2gtk-4.1 (ou 4.0).' ;;
        inst.noWebkit) f='Le paquet WebKitGTK n'"'"'est pas installé : l'"'"'interface s'"'"'ouvrira dans un navigateur.' ;;
        inst.noPwCli) f='pw-cli est introuvable. Votre système audio est-il bien PipeWire ?' ;;
        inst.systemPkg) f='Une version empaquetée d'"'"'Aqulify est également installée (/usr/bin). Cette installation (~/.local) est désormais prioritaire ; pour utiliser la version empaquetée, lancez ./uninstall.sh par la suite.' ;;
        inst.step2) f='[2/6] Compilation...' ;;
        inst.rnnSystem) f='RNNoise (réduction du bruit de la voix) : paquet système.' ;;
        inst.rnnDownload) f='Téléchargement du modèle RNNoise (archive Ubuntu, environ 19 Mo)...' ;;
        inst.rnnOk) f='RNNoise (réduction du bruit de la voix) : intégré.' ;;
        inst.rnnFail) f='Impossible de télécharger le modèle RNNoise : la réduction du bruit de la voix utilise la méthode par bandes intégrée.' ;;
        inst.engineFail) f='La compilation du moteur a échoué.' ;;
        inst.engineOk) f='Moteur : build/aqulify-engine' ;;
        inst.guiOk) f='Interface de fenêtre : build/aqulify-gui' ;;
        inst.guiFail1) f='L'"'"'interface de fenêtre (WebKitGTK) n'"'"'a pas pu être compilée (détails : build/gui-build.log).' ;;
        inst.guiFail2) f='Solution de repli : fenêtre d'"'"'un navigateur basé sur Chromium, ou à défaut le navigateur par défaut.' ;;
        inst.guiFail3) f='Solution : installez les paquets de développement gtk3 et webkit2gtk-4.1, puis relancez ./install.sh.' ;;
        inst.step3) f='[3/6] Copie des fichiers (%s)...' ;;
        inst.legacy) f='Suppression de l'"'"'ancienne installation d'"'"'AetherAudio (les paramètres sont conservés)...' ;;
        inst.filesOk) f='Fichiers installés.' ;;
        inst.step4) f='[4/6] Configuration du démarrage automatique (service utilisateur systemd)...' ;;
        inst.unitDesc) f='Moteur DSP Aqulify (sortie audio virtuelle « Aqulify »)' ;;
        inst.serviceOk) f='Le moteur fonctionne et démarre automatiquement à l'"'"'ouverture de session.' ;;
        inst.serviceFail) f='Le service utilisateur systemd n'"'"'est pas disponible. Démarrez-le manuellement : %s &' ;;
        inst.step5) f='[5/6] Entrée dans le menu...' ;;
        inst.desktopGeneric) f='Égaliseur et analyseur à cascade acoustique' ;;
        inst.desktopComment) f='EQ paramétrique, convolueur, crossfeed et limiteur True Peak pour PipeWire avec analyseur cascade 3D en direct' ;;
        inst.menuOk) f='Dans le menu et sur le bureau : Aqulify' ;;
        inst.step6) f='[6/6] Sortie audio par défaut' ;;
        inst.defaultExplain) f='L'"'"'audio des applications passe par Aqulify quand « Aqulify » est le périphérique de sortie.' ;;
        inst.legacyDefault) f='L'"'"'ancienne version AetherAudio était la sortie par défaut : Aqulify prend le relais.' ;;
        inst.askDefault) f='La définir par défaut maintenant ? [y/N] ' ;;
        inst.defaultOk) f='« Aqulify » est maintenant la sortie par défaut.' ;;
        inst.defaultFail) f='L'"'"'opération a échoué ; définissez-la dans les paramètres Son (Sortie : Aqulify).' ;;
        inst.defaultLater) f='Vous pourrez la sélectionner plus tard dans les paramètres Son (Sortie : Aqulify).' ;;
        inst.done) f='Terminé ! Aqulify est installé.' ;;
        inst.sumGui) f='Interface :     menu → Aqulify  (ou : aqulify-gui)' ;;
        inst.sumState) f='État moteur :   systemctl --user status aqulify' ;;
        inst.sumLog) f='Journal :       journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Désinstaller :  ./uninstall.sh' ;;
        esac ;;
    zh) case "$2" in
        common.errorPrefix) f='[Error]' ;;
        inst.title) f='Aqulify – Linux 安装' ;;
        inst.noRoot) f='请勿以 root 身份运行！请以你自己的用户身份启动（安装软件包时会提示输入 sudo 密码）。' ;;
        inst.step1) f='[1/6] 正在安装所需软件包...' ;;
        inst.noOsRelease) f='无法确定发行版（/etc/os-release）。' ;;
        inst.unknownDistro1) f='未知发行版（%s）。需要：C/C++20 编译器、cmake、pkg-config、libpipewire-0.3 及 fftw3（float）开发包；' ;;
        inst.unknownDistro2) f='应用程序窗口需要：gtk3 与 webkit2gtk-4.1（或 4.0）开发包。' ;;
        inst.noWebkit) f='未安装 WebKitGTK 软件包：界面将在浏览器中打开。' ;;
        inst.noPwCli) f='未找到 pw-cli。你的声音系统确实是 PipeWire 吗？' ;;
        inst.systemPkg) f='系统还安装了打包版 Aqulify（/usr/bin）。此安装（~/.local）自此具有更高优先级；如需使用打包版，请在此后运行 ./uninstall.sh。' ;;
        inst.step2) f='[2/6] 正在编译...' ;;
        inst.rnnSystem) f='RNNoise（语音降噪）：系统软件包。' ;;
        inst.rnnDownload) f='正在下载 RNNoise 模型（Ubuntu 归档，约 19 MB）...' ;;
        inst.rnnOk) f='RNNoise（语音降噪）：内置。' ;;
        inst.rnnFail) f='无法下载 RNNoise 模型：语音降噪将使用内置分频段算法。' ;;
        inst.engineFail) f='编译引擎失败。' ;;
        inst.engineOk) f='引擎：build/aqulify-engine' ;;
        inst.guiOk) f='应用程序窗口界面：build/aqulify-gui' ;;
        inst.guiFail1) f='应用程序窗口（WebKitGTK）界面编译失败（详情：build/gui-build.log）。' ;;
        inst.guiFail2) f='回退方案：基于 Chromium 的浏览器应用窗口，若没有则使用默认浏览器。' ;;
        inst.guiFail3) f='解决办法：安装 gtk3 和 webkit2gtk-4.1 开发包，然后重新运行 ./install.sh。' ;;
        inst.step3) f='[3/6] 正在复制文件（%s）...' ;;
        inst.legacy) f='正在移除旧版 AetherAudio 安装（设置将继承）...' ;;
        inst.filesOk) f='文件已就位。' ;;
        inst.step4) f='[4/6] 正在配置自动启动（systemd 用户服务）...' ;;
        inst.unitDesc) f='Aqulify DSP 引擎（虚拟“Aqulify”音频输出）' ;;
        inst.serviceOk) f='引擎正在运行，并将在登录时自动启动。' ;;
        inst.serviceFail) f='systemd 用户服务不可用。请手动启动：%s &' ;;
        inst.step5) f='[5/6] 正在添加菜单项...' ;;
        inst.desktopGeneric) f='均衡器与声学瀑布图分析器' ;;
        inst.desktopComment) f='适用于 PipeWire 的参数 EQ、卷积器、crossfeed 及 True Peak 限幅器，配备实时 3D 瀑布图分析器' ;;
        inst.menuOk) f='已在菜单和桌面上添加：Aqulify' ;;
        inst.step6) f='[6/6] 默认音频输出' ;;
        inst.defaultExplain) f='当“Aqulify”设为输出设备时，应用程序音频将流经 Aqulify。' ;;
        inst.legacyDefault) f='原先的 AetherAudio 是默认输出：Aqulify 已接管。' ;;
        inst.askDefault) f='现在将其设为默认输出吗？[y/N] ' ;;
        inst.defaultOk) f='“Aqulify”现已成为默认输出。' ;;
        inst.defaultFail) f='设置未生效；请在“声音”设置中手动指定（输出：Aqulify）。' ;;
        inst.defaultLater) f='你稍后可以在“声音”设置中进行选择（输出：Aqulify）。' ;;
        inst.done) f='大功告成！Aqulify 已安装完毕。' ;;
        inst.sumGui) f='界面：        菜单 → Aqulify  （或：aqulify-gui）' ;;
        inst.sumState) f='引擎状态：    systemctl --user status aqulify' ;;
        inst.sumLog) f='日志：        journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='卸载：        ./uninstall.sh' ;;
        esac ;;
    de) case "$2" in
        common.errorPrefix) f='[Fehler]' ;;
        inst.title) f='Aqulify – Installation unter Linux' ;;
        inst.noRoot) f='Nicht als root ausführen! Bitte als normaler Benutzer starten (für Pakete wird nach dem sudo-Passwort gefragt).' ;;
        inst.step1) f='[1/6] Erforderliche Pakete werden installiert...' ;;
        inst.noOsRelease) f='Distribution kann nicht ermittelt werden (/etc/os-release).' ;;
        inst.unknownDistro1) f='Unbekannte Distribution (%s). Erforderlich: C/C++20-Compiler, cmake, pkg-config, Entwicklungspakete für libpipewire-0.3 und fftw3 (float);' ;;
        inst.unknownDistro2) f='für das Anwendungsfenster: Entwicklungspakete für gtk3 und webkit2gtk-4.1 (oder 4.0).' ;;
        inst.noWebkit) f='Das WebKitGTK-Paket wurde nicht installiert: Die Benutzeroberfläche wird im Browser geöffnet.' ;;
        inst.noPwCli) f='pw-cli wurde nicht gefunden. Ist PipeWire wirklich Ihr Soundsystem?' ;;
        inst.systemPkg) f='Ein paketiertes Aqulify ist ebenfalls installiert (/usr/bin). Diese Installation (~/.local) hat ab jetzt Vorrang; um die paketierte Version zu nutzen, führen Sie danach ./uninstall.sh aus.' ;;
        inst.step2) f='[2/6] Erstellung...' ;;
        inst.rnnSystem) f='RNNoise (Sprach-Rauschunterdrückung): Systempaket.' ;;
        inst.rnnDownload) f='RNNoise-Modell wird heruntergeladen (Ubuntu-Archiv, ca. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (Sprach-Rauschunterdrückung): integriert.' ;;
        inst.rnnFail) f='Das RNNoise-Modell konnte nicht heruntergeladen werden: Sprach-Rauschunterdrückung nutzt die integrierte bandweise Methode.' ;;
        inst.engineFail) f='Erstellung der Engine fehlgeschlagen.' ;;
        inst.engineOk) f='Engine: build/aqulify-engine' ;;
        inst.guiOk) f='Anwendungsfenster-Oberfläche: build/aqulify-gui' ;;
        inst.guiFail1) f='Die Oberfläche im Anwendungsfenster (WebKitGTK) konnte nicht kompiliert werden (Details: build/gui-build.log).' ;;
        inst.guiFail2) f='Ausweichlösung: Anwendungsfenster eines Chromium-basierten Browsers oder der Standard-Webbrowser.' ;;
        inst.guiFail3) f='Lösung: Installieren Sie die Entwicklungspakete gtk3 und webkit2gtk-4.1 und führen Sie ./install.sh erneut aus.' ;;
        inst.step3) f='[3/6] Dateien werden kopiert (%s)...' ;;
        inst.legacy) f='Vorherige AetherAudio-Installation wird entfernt (Einstellungen werden übernommen)...' ;;
        inst.filesOk) f='Dateien erfolgreich kopiert.' ;;
        inst.step4) f='[4/6] Automatischer Start wird eingerichtet (systemd-Benutzerdienst)...' ;;
        inst.unitDesc) f='Aqulify DSP-Engine (virtueller Audioausgang "Aqulify")' ;;
        inst.serviceOk) f='Die Engine läuft und startet automatisch bei der Anmeldung.' ;;
        inst.serviceFail) f='Der systemd-Benutzerdienst ist nicht verfügbar. Bitte manuell starten: %s &' ;;
        inst.step5) f='[5/6] Menüeintrag...' ;;
        inst.desktopGeneric) f='Equalizer und akustischer Wasserfall-Analysator' ;;
        inst.desktopComment) f='Parametrischer EQ, Convolver, Crossfeed und True-Peak-Limiter für PipeWire mit 3D-Echtzeit-Wasserfallanalysator' ;;
        inst.menuOk) f='Im Menü und auf dem Desktop: Aqulify' ;;
        inst.step6) f='[6/6] Standard-Audioausgang' ;;
        inst.defaultExplain) f='Der Ton von Anwendungen wird über Aqulify wiedergegeben, wenn "Aqulify" als Ausgabegerät festgelegt ist.' ;;
        inst.legacyDefault) f='Zuvor war AetherAudio der Standardausgang: Aqulify übernimmt.' ;;
        inst.askDefault) f='Jetzt als Standardausgang festlegen? [j/N] ' ;;
        inst.defaultOk) f='"Aqulify" ist jetzt der Standard-Audioausgang.' ;;
        inst.defaultFail) f='Festlegung fehlgeschlagen; bitte in den Sound-Einstellungen festlegen (Ausgang: Aqulify).' ;;
        inst.defaultLater) f='Sie können dies später in den Sound-Einstellungen auswählen (Ausgang: Aqulify).' ;;
        inst.done) f='Fertig! Aqulify wurde erfolgreich installiert.' ;;
        inst.sumGui) f='Oberfläche:     Menü → Aqulify  (oder: aqulify-gui)' ;;
        inst.sumState) f='Engine-Status:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Protokoll:      journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Deinstallation: ./uninstall.sh' ;;
        esac ;;
    it) case "$2" in
        common.errorPrefix) f='[Errore]' ;;
        inst.title) f='Aqulify – installazione su Linux' ;;
        inst.noRoot) f='Non eseguire come root! Avvia come utente standard (verrà chiesta la password sudo per i pacchetti).' ;;
        inst.step1) f='[1/6] Installazione dei pacchetti necessari...' ;;
        inst.noOsRelease) f='Impossibile determinare la distribuzione (/etc/os-release).' ;;
        inst.unknownDistro1) f='Distribuzione sconosciuta (%s). Richiesti: compilatore C/C++20, cmake, pkg-config, pacchetti di sviluppo per libpipewire-0.3 e fftw3 (float);' ;;
        inst.unknownDistro2) f='per la finestra dell'"'"'applicazione: pacchetti di sviluppo per gtk3 e webkit2gtk-4.1 (o 4.0).' ;;
        inst.noWebkit) f='Il pacchetto WebKitGTK non è installato: l'"'"'interfaccia si aprirà nel browser.' ;;
        inst.noPwCli) f='pw-cli non trovato. Il tuo sistema audio è davvero PipeWire?' ;;
        inst.systemPkg) f='È installata anche una versione di sistema di Aqulify (/usr/bin). Questa installazione (~/.local) ha la priorità d'"'"'ora in avanti; per usare quella di sistema, esegui successivamente ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Compilazione...' ;;
        inst.rnnSystem) f='RNNoise (riduzione del rumore vocale): pacchetto di sistema.' ;;
        inst.rnnDownload) f='Download del modello RNNoise (archivio Ubuntu, circa 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (riduzione del rumore vocale): integrato.' ;;
        inst.rnnFail) f='Impossibile scaricare il modello RNNoise: la riduzione del rumore userà il metodo a bande integrato.' ;;
        inst.engineFail) f='Compilazione del motore non riuscita.' ;;
        inst.engineOk) f='Motore: build/aqulify-engine' ;;
        inst.guiOk) f='Interfaccia applicativa: build/aqulify-gui' ;;
        inst.guiFail1) f='Compilazione dell'"'"'interfaccia a finestra (WebKitGTK) fallita (dettagli: build/gui-build.log).' ;;
        inst.guiFail2) f='Ripiego: finestra app di un browser basato su Chromium, o se non presente, il browser predefinito.' ;;
        inst.guiFail3) f='Risoluzione: installa i pacchetti di sviluppo gtk3 e webkit2gtk-4.1, poi riesegui ./install.sh.' ;;
        inst.step3) f='[3/6] Copia dei file (%s)...' ;;
        inst.legacy) f='Rimozione della precedente installazione di AetherAudio (le impostazioni vengono mantenute)...' ;;
        inst.filesOk) f='File posizionati correttamente.' ;;
        inst.step4) f='[4/6] Configurazione avvio automatico (servizio utente systemd)...' ;;
        inst.unitDesc) f='Motore DSP Aqulify (uscita audio virtuale "Aqulify")' ;;
        inst.serviceOk) f='Il motore è in esecuzione e si avvierà automaticamente all'"'"'accesso.' ;;
        inst.serviceFail) f='Il servizio utente systemd non è disponibile. Avvialo manualmente: %s &' ;;
        inst.step5) f='[5/6] Voce di menu...' ;;
        inst.desktopGeneric) f='Equalizzatore e analizzatore a cascata acustica' ;;
        inst.desktopComment) f='EQ parametrico, convolutore, crossfeed e true peak limiter per PipeWire con analizzatore 3D waterfall in tempo reale' ;;
        inst.menuOk) f='Nel menu e sul desktop: Aqulify' ;;
        inst.step6) f='[6/6] Uscita audio predefinita' ;;
        inst.defaultExplain) f='L'"'"'audio delle applicazioni passa attraverso Aqulify quando "Aqulify" è il dispositivo di uscita.' ;;
        inst.legacyDefault) f='La precedente installazione di AetherAudio era l'"'"'uscita predefinita: Aqulify ne prende il posto.' ;;
        inst.askDefault) f='Impostare ora come uscita predefinita? [s/N] ' ;;
        inst.defaultOk) f='"Aqulify" è ora l'"'"'uscita predefinita.' ;;
        inst.defaultFail) f='Operazione non riuscita; imposta manualmente nelle impostazioni audio (Uscita: Aqulify).' ;;
        inst.defaultLater) f='Potrai selezionarlo in seguito nelle impostazioni audio (Uscita: Aqulify).' ;;
        inst.done) f='Fatto! Aqulify è stato installato.' ;;
        inst.sumGui) f='Interfaccia:    menu → Aqulify  (oppure: aqulify-gui)' ;;
        inst.sumState) f='Stato motore:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Registro:       journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Disinstallazione: ./uninstall.sh' ;;
        esac ;;
    nl) case "$2" in
        common.errorPrefix) f='[Fout]' ;;
        inst.title) f='Aqulify – installatie op Linux' ;;
        inst.noRoot) f='Niet als root uitvoeren! Start als uw eigen gebruiker (er wordt om het sudo-wachtwoord gevraagd voor pakketten).' ;;
        inst.step1) f='[1/6] Vereiste pakketten installeren...' ;;
        inst.noOsRelease) f='Kan de distributie niet vaststellen (/etc/os-release).' ;;
        inst.unknownDistro1) f='Onbekende distributie (%s). Vereist: C/C++20-compiler, cmake, pkg-config, ontwikkelpakketten voor libpipewire-0.3 en fftw3 (float);' ;;
        inst.unknownDistro2) f='voor het applicatievenster: ontwikkelpakketten voor gtk3 en webkit2gtk-4.1 (of 4.0).' ;;
        inst.noWebkit) f='Het WebKitGTK-pakket is niet geïnstalleerd: de interface wordt geopend in een browser.' ;;
        inst.noPwCli) f='pw-cli niet gevonden. Is PipeWire werkelijk uw geluidssysteem?' ;;
        inst.systemPkg) f='Er is ook een pakketversie van Aqulify geïnstalleerd (/usr/bin). Deze installatie (~/.local) heeft vanaf nu voorrang; om de pakketversie te gebruiken, voert u daarna ./uninstall.sh uit.' ;;
        inst.step2) f='[2/6] Compileren...' ;;
        inst.rnnSystem) f='RNNoise (spraakruisonderdrukking): systeempakket.' ;;
        inst.rnnDownload) f='RNNoise-model downloaden (Ubuntu-archief, circa 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (spraakruisonderdrukking): ingebouwd.' ;;
        inst.rnnFail) f='Het RNNoise-model kon niet worden gedownload: spraakruisonderdrukking gebruikt de ingebouwde bandsgewijze methode.' ;;
        inst.engineFail) f='Compileren van de engine is mislukt.' ;;
        inst.engineOk) f='Engine: build/aqulify-engine' ;;
        inst.guiOk) f='Applicatievenster-interface: build/aqulify-gui' ;;
        inst.guiFail1) f='De interface in het applicatievenster (WebKitGTK) kon niet worden gecompileerd (details: build/gui-build.log).' ;;
        inst.guiFail2) f='Terugval: applicatievenster van een Chromium-gebaseerde browser of anders de standaardbrowser.' ;;
        inst.guiFail3) f='Oplossing: installeer de ontwikkelpakketten gtk3 en webkit2gtk-4.1 en voer ./install.sh opnieuw uit.' ;;
        inst.step3) f='[3/6] Bestanden kopiëren (%s)...' ;;
        inst.legacy) f='Vorige AetherAudio-installatie verwijderen (instellingen blijven behouden)...' ;;
        inst.filesOk) f='Bestanden op hun plaats.' ;;
        inst.step4) f='[4/6] Automatisch starten instellen (systemd-gebruikersservice)...' ;;
        inst.unitDesc) f='Aqulify DSP-engine (virtuele "Aqulify" geluidsuitvoer)' ;;
        inst.serviceOk) f='De engine draait en start automatisch bij het inloggen.' ;;
        inst.serviceFail) f='De systemd-gebruikersservice is niet beschikbaar. Start handmatig: %s &' ;;
        inst.step5) f='[5/6] Menu-item...' ;;
        inst.desktopGeneric) f='Equalizer en akoestische waterval-analyser' ;;
        inst.desktopComment) f='Parametrische EQ, convolver, crossfeed en true peak limiter voor PipeWire met live 3D-watervalanalyser' ;;
        inst.menuOk) f='In het menu en op het bureaublad: Aqulify' ;;
        inst.step6) f='[6/6] Standaard audio-uitvoer' ;;
        inst.defaultExplain) f='Geluid van toepassingen loopt via Aqulify wanneer "Aqulify" het uitvoerapparaat is.' ;;
        inst.legacyDefault) f='Voorheen was AetherAudio de standaarduitvoer: Aqulify neemt dit over.' ;;
        inst.askDefault) f='Nu instellen als standaarduitvoer? [j/N] ' ;;
        inst.defaultOk) f='"Aqulify" is nu de standaard audio-uitvoer.' ;;
        inst.defaultFail) f='Instellen mislukt; stel handmatig in bij Geluidsinstellingen (Uitvoer: Aqulify).' ;;
        inst.defaultLater) f='U kunt dit later kiezen in de Geluidsinstellingen (Uitvoer: Aqulify).' ;;
        inst.done) f='Klaar! Aqulify is geïnstalleerd.' ;;
        inst.sumGui) f='Interface:      menu → Aqulify  (of: aqulify-gui)' ;;
        inst.sumState) f='Engine-status:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Logboek:        journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Verwijderen:    ./uninstall.sh' ;;
        esac ;;
    pl) case "$2" in
        common.errorPrefix) f='[Błąd]' ;;
        inst.title) f='Aqulify – instalacja w systemie Linux' ;;
        inst.noRoot) f='Nie uruchamiaj jako root! Uruchom jako zwykły użytkownik (hasło sudo będzie potrzebne do pakietów).' ;;
        inst.step1) f='[1/6] Instalowanie wymaganych pakietów...' ;;
        inst.noOsRelease) f='Nie można ustalić dystrybucji (/etc/os-release).' ;;
        inst.unknownDistro1) f='Nieznana dystrybucja (%s). Wymagane: kompilator C/C++20, cmake, pkg-config, pakiety deweloperskie libpipewire-0.3 i fftw3 (float);' ;;
        inst.unknownDistro2) f='dla okna aplikacji: pakiety deweloperskie gtk3 i webkit2gtk-4.1 (lub 4.0).' ;;
        inst.noWebkit) f='Pakiet WebKitGTK nie został zainstalowany: interfejs otworzy się w przeglądarce.' ;;
        inst.noPwCli) f='Nie znaleziono pw-cli. Czy Twoim serwerem dźwięku na pewno jest PipeWire?' ;;
        inst.systemPkg) f='W systemie zainstalowany jest również Aqulify z pakietu (/usr/bin). Ta instalacja (~/.local) ma od teraz pierwszeństwo; aby używać wersji z pakietu, uruchom potem ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Kompilacja...' ;;
        inst.rnnSystem) f='RNNoise (redukcja szumu mowy): pakiet systemowy.' ;;
        inst.rnnDownload) f='Pobieranie modelu RNNoise (archiwum Ubuntu, ok. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (redukcja szumu mowy): wbudowano.' ;;
        inst.rnnFail) f='Nie udało się pobrać modelu RNNoise: redukcja szumu mowy użyje wbudowanej metody pasmowej.' ;;
        inst.engineFail) f='Kompilacja silnika nie powiodła się.' ;;
        inst.engineOk) f='Silnik: build/aqulify-engine' ;;
        inst.guiOk) f='Interfejs okienkowy: build/aqulify-gui' ;;
        inst.guiFail1) f='Kompilacja okna aplikacji (WebKitGTK) nie powiodła się (szczegóły: build/gui-build.log).' ;;
        inst.guiFail2) f='Opcja zapasowa: okno aplikacji przeglądarki opartej na Chromium lub przeglądarka domyślna.' ;;
        inst.guiFail3) f='Rozwiązanie: zainstaluj pakiety deweloperskie gtk3 i webkit2gtk-4.1, a następnie uruchom ponownie ./install.sh.' ;;
        inst.step3) f='[3/6] Kopiowanie plików (%s)...' ;;
        inst.legacy) f='Usuwanie poprzedniej instalacji AetherAudio (ustawienia zostaną przeniesione)...' ;;
        inst.filesOk) f='Pliki na swoim miejscu.' ;;
        inst.step4) f='[4/6] Konfiguracja automatycznego startu (usługa użytkownika systemd)...' ;;
        inst.unitDesc) f='Silnik DSP Aqulify (wirtualne wyjście audio "Aqulify")' ;;
        inst.serviceOk) f='Silnik działa i uruchamia się automatycznie po zalogowaniu.' ;;
        inst.serviceFail) f='Usługa użytkownika systemd nie jest dostępna. Uruchom ręcznie: %s &' ;;
        inst.step5) f='[5/6] Wpis w menu...' ;;
        inst.desktopGeneric) f='Korektor i analizator wodospadowy akustyki' ;;
        inst.desktopComment) f='Parametryczny korektor, konwolwer, crossfeed i limiter true-peak dla PipeWire z analizatorem wodospadowym 3D na żywo' ;;
        inst.menuOk) f='W menu i na pulpicie: Aqulify' ;;
        inst.step6) f='[6/6] Domyślne wyjście audio' ;;
        inst.defaultExplain) f='Dźwięk aplikacji przechodzi przez Aqulify, gdy "Aqulify" jest wybranym urządzeniem wyjściowym.' ;;
        inst.legacyDefault) f='Poprzednio domyślnym wyjściem było AetherAudio: Aqulify przejmuje tę rolę.' ;;
        inst.askDefault) f='Ustawić teraz jako wyjście domyślne? [t/N] ' ;;
        inst.defaultOk) f='"Aqulify" jest teraz domyślnym wyjściem audio.' ;;
        inst.defaultFail) f='Nie powiodło się; ustaw w Ustawieniach dźwięku (Wyjście: Aqulify).' ;;
        inst.defaultLater) f='Możesz to wybrać później w Ustawieniach dźwięku (Wyjście: Aqulify).' ;;
        inst.done) f='Gotowe! Aqulify został zainstalowany.' ;;
        inst.sumGui) f='Interfejs:      menu → Aqulify  (lub: aqulify-gui)' ;;
        inst.sumState) f='Stan silnika:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Dziennik:       journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Deinstalacja:   ./uninstall.sh' ;;
        esac ;;
    ru) case "$2" in
        common.errorPrefix) f='[Ошибка]' ;;
        inst.title) f='Aqulify – установка в Linux' ;;
        inst.noRoot) f='Не запускайте от имени root! Запустите от обычного пользователя (для пакетов потребуется пароль sudo).' ;;
        inst.step1) f='[1/6] Установка необходимых пакетов...' ;;
        inst.noOsRelease) f='Не удалось определить дистрибутив (/etc/os-release).' ;;
        inst.unknownDistro1) f='Неизвестный дистрибутив (%s). Требуются: компилятор C/C++20, cmake, pkg-config, пакеты разработки libpipewire-0.3 и fftw3 (float);' ;;
        inst.unknownDistro2) f='для окна приложения: пакеты разработки gtk3 и webkit2gtk-4.1 (или 4.0).' ;;
        inst.noWebkit) f='Пакет WebKitGTK не установлен: интерфейс откроется в браузере.' ;;
        inst.noPwCli) f='Утилита pw-cli не найдена. Ваша звуковая система точно PipeWire?' ;;
        inst.systemPkg) f='В системе также установлен пакет Aqulify (/usr/bin). Эта установка (~/.local) теперь имеет приоритет; чтобы использовать пакетную версию, выполните затем ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Сборка...' ;;
        inst.rnnSystem) f='RNNoise (шумоподавление речи): системный пакет.' ;;
        inst.rnnDownload) f='Загрузка модели RNNoise (архив Ubuntu, около 19 МБ)...' ;;
        inst.rnnOk) f='RNNoise (шумоподавление речи): встроено.' ;;
        inst.rnnFail) f='Не удалось загрузить модель RNNoise: шумоподавление речи будет использовать встроенный полосовой метод.' ;;
        inst.engineFail) f='Сборка аудиодвижка не удалась.' ;;
        inst.engineOk) f='Движок: build/aqulify-engine' ;;
        inst.guiOk) f='Оконный интерфейс: build/aqulify-gui' ;;
        inst.guiFail1) f='Сборка оконного интерфейса (WebKitGTK) не удалась (подробности: build/gui-build.log).' ;;
        inst.guiFail2) f='Резервный вариант: окно приложения браузера на базе Chromium или браузер по умолчанию.' ;;
        inst.guiFail3) f='Решение: установите пакеты разработки gtk3 и webkit2gtk-4.1, затем снова запустите ./install.sh.' ;;
        inst.step3) f='[3/6] Копирование файлов (%s)...' ;;
        inst.legacy) f='Удаление предыдущей установки AetherAudio (настройки переносятся)...' ;;
        inst.filesOk) f='Файлы скопированы.' ;;
        inst.step4) f='[4/6] Настройка автозапуска (пользовательская служба systemd)...' ;;
        inst.unitDesc) f='DSP-движок Aqulify (виртуальный аудиовыход "Aqulify")' ;;
        inst.serviceOk) f='Движок работает и запускается автоматически при входе в систему.' ;;
        inst.serviceFail) f='Пользовательская служба systemd недоступна. Запустите вручную: %s &' ;;
        inst.step5) f='[5/6] Пункт меню...' ;;
        inst.desktopGeneric) f='Эквалайзер и акустический спектральный водопад' ;;
        inst.desktopComment) f='Параметрический эквалайзер, конволвер, кроссфид и true-peak лимитер для PipeWire с 3D-водопадом в реальном времени' ;;
        inst.menuOk) f='В меню и на рабочем столе: Aqulify' ;;
        inst.step6) f='[6/6] Аудиовыход по умолчанию' ;;
        inst.defaultExplain) f='Звук приложений идет через Aqulify, когда "Aqulify" выбран устройством вывода.' ;;
        inst.legacyDefault) f='Ранее выходом по умолчанию был AetherAudio: Aqulify принимает управление.' ;;
        inst.askDefault) f='Назначить выходом по умолчанию прямо сейчас? [д/Н] ' ;;
        inst.defaultOk) f='"Aqulify" теперь является аудиовыходом по умолчанию.' ;;
        inst.defaultFail) f='Не удалось установить; выберите вручную в настройках звука (Выход: Aqulify).' ;;
        inst.defaultLater) f='Вы сможете выбрать его позже в настройках звука (Выход: Aqulify).' ;;
        inst.done) f='Готово! Aqulify успешно установлен.' ;;
        inst.sumGui) f='Интерфейс:      меню → Aqulify  (или: aqulify-gui)' ;;
        inst.sumState) f='Статус движка:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Журнал:         journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Удаление:       ./uninstall.sh' ;;
        esac ;;
    pt) case "$2" in
        common.errorPrefix) f='[Erro]' ;;
        inst.title) f='Aqulify – instalação no Linux' ;;
        inst.noRoot) f='Não execute como root! Inicie como usuário comum (será solicitada a senha do sudo para os pacotes).' ;;
        inst.step1) f='[1/6] Instalando os pacotes necessários...' ;;
        inst.noOsRelease) f='Não foi possível identificar a distribuição (/etc/os-release).' ;;
        inst.unknownDistro1) f='Distribuição desconhecida (%s). Necessário: compilador C/C++20, cmake, pkg-config, pacotes de desenvolvimento libpipewire-0.3 e fftw3 (float);' ;;
        inst.unknownDistro2) f='para a janela da aplicação: pacotes de desenvolvimento gtk3 e webkit2gtk-4.1 (ou 4.0).' ;;
        inst.noWebkit) f='O pacote WebKitGTK não foi instalado: a interface será aberta no navegador.' ;;
        inst.noPwCli) f='pw-cli não encontrado. O PipeWire é realmente o seu sistema de som?' ;;
        inst.systemPkg) f='Um Aqulify empacotado também está instalado (/usr/bin). Esta instalação (~/.local) tem precedência; para usar a versão empacotada, execute ./uninstall.sh depois.' ;;
        inst.step2) f='[2/6] Compilação...' ;;
        inst.rnnSystem) f='RNNoise (redução de ruído de voz): pacote do sistema.' ;;
        inst.rnnDownload) f='Baixando modelo RNNoise (arquivo Ubuntu, aprox. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (redução de ruído de voz): integrado.' ;;
        inst.rnnFail) f='Falha ao baixar o modelo RNNoise: a redução de ruído de voz usará o método integrado por faixas.' ;;
        inst.engineFail) f='Falha na compilação do motor.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Interface de janela: build/aqulify-gui' ;;
        inst.guiFail1) f='Não foi possível compilar a interface de janela (WebKitGTK) (detalhes: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativa: janela de aplicação de navegador baseado em Chromium ou o navegador padrão.' ;;
        inst.guiFail3) f='Solução: instale os pacotes de desenvolvimento gtk3 e webkit2gtk-4.1 e execute ./install.sh novamente.' ;;
        inst.step3) f='[3/6] Copiando arquivos (%s)...' ;;
        inst.legacy) f='Removendo instalação anterior do AetherAudio (configurações preservadas)...' ;;
        inst.filesOk) f='Arquivos copiados com sucesso.' ;;
        inst.step4) f='[4/6] Configurando inicialização automática (serviço de usuário systemd)...' ;;
        inst.unitDesc) f='Motor DSP Aqulify (saída de áudio virtual "Aqulify")' ;;
        inst.serviceOk) f='O motor está em execução e iniciará automaticamente no login.' ;;
        inst.serviceFail) f='Serviço de usuário systemd indisponível. Inicie manualmente: %s &' ;;
        inst.step5) f='[5/6] Entrada no menu...' ;;
        inst.desktopGeneric) f='Equalizador e analisador acústico em cascata' ;;
        inst.desktopComment) f='EQ paramétrico, convolucionador, crossfeed e limitador true peak para PipeWire com analisador em cascata 3D em tempo real' ;;
        inst.menuOk) f='No menu e na área de trabalho: Aqulify' ;;
        inst.step6) f='[6/6] Saída de áudio padrão' ;;
        inst.defaultExplain) f='O áudio dos aplicativos é reproduzido pelo Aqulify quando "Aqulify" estiver definido como dispositivo de saída.' ;;
        inst.legacyDefault) f='Anteriormente o AetherAudio era a saída padrão: o Aqulify assume.' ;;
        inst.askDefault) f='Definir como saída padrão agora? [s/N] ' ;;
        inst.defaultOk) f='"Aqulify" agora é a saída de áudio padrão.' ;;
        inst.defaultFail) f='Falha ao definir; selecione nas configurações de som (Saída: Aqulify).' ;;
        inst.defaultLater) f='Você pode selecioná-lo mais tarde nas configurações de som (Saída: Aqulify).' ;;
        inst.done) f='Concluído! O Aqulify foi instalado com sucesso.' ;;
        inst.sumGui) f='Interface:      Menu → Aqulify  (ou: aqulify-gui)' ;;
        inst.sumState) f='Status motor:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Logs:           journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Desinstalação:  ./uninstall.sh' ;;
        esac ;;
    ro) case "$2" in
        common.errorPrefix) f='[Eroare]' ;;
        inst.title) f='Aqulify – instalare pe Linux' ;;
        inst.noRoot) f='Nu rulați ca root! Porniți ca utilizator obișnuit (se va cere parola sudo pentru pachete).' ;;
        inst.step1) f='[1/6] Se instalează pachetele necesare...' ;;
        inst.noOsRelease) f='Nu se poate identifica distribuția (/etc/os-release).' ;;
        inst.unknownDistro1) f='Distribuție necunoscută (%s). Necesar: compilator C/C++20, cmake, pkg-config, pachete de dezvoltare libpipewire-0.3 și fftw3 (float);' ;;
        inst.unknownDistro2) f='pentru fereastra aplicației: pachete de dezvoltare gtk3 și webkit2gtk-4.1 (sau 4.0).' ;;
        inst.noWebkit) f='Pachetul WebKitGTK nu a fost instalat: interfața se va deschide în browser.' ;;
        inst.noPwCli) f='pw-cli nu a fost găsit. PipeWire este într-adevăr sistemul dvs. de sunet?' ;;
        inst.systemPkg) f='Este instalat și un Aqulify din pachet (/usr/bin). Această instalare (~/.local) are prioritate; pentru a utiliza versiunea împachetată, rulați ./uninstall.sh ulterior.' ;;
        inst.step2) f='[2/6] Compilare...' ;;
        inst.rnnSystem) f='RNNoise (reducerea zgomotului vocal): pachet de sistem.' ;;
        inst.rnnDownload) f='Descărcare model RNNoise (arhivă Ubuntu, aprox. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (reducerea zgomotului vocal): integrat.' ;;
        inst.rnnFail) f='Descărcarea modelului RNNoise a eșuat: reducerea zgomotului vocal utilizează metoda integrată pe benzi.' ;;
        inst.engineFail) f='Compilarea motorului a eșuat.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Interfață fereastră aplicație: build/aqulify-gui' ;;
        inst.guiFail1) f='Interfața ferestrei aplicației (WebKitGTK) nu a putut fi compilată (detalii: build/gui-build.log).' ;;
        inst.guiFail2) f='Soluție de rezervă: fereastră de aplicație bazată pe Chromium sau browserul web implicit.' ;;
        inst.guiFail3) f='Soluție: instalați pachetele de dezvoltare gtk3 și webkit2gtk-4.1 și rulați din nou ./install.sh.' ;;
        inst.step3) f='[3/6] Se copiază fișierele (%s)...' ;;
        inst.legacy) f='Se elimină instalarea anterioară AetherAudio (setările se păstrează)...' ;;
        inst.filesOk) f='Fișiere copiate cu succes.' ;;
        inst.step4) f='[4/6] Se configurează pornirea automată (serviciu de utilizator systemd)...' ;;
        inst.unitDesc) f='Motor DSP Aqulify (ieșire audio virtuală „Aqulify”)' ;;
        inst.serviceOk) f='Motorul rulează și pornește automat la autentificare.' ;;
        inst.serviceFail) f='Serviciul de utilizator systemd nu este disponibil. Porniți manual: %s &' ;;
        inst.step5) f='[5/6] Intrare în meniu...' ;;
        inst.desktopGeneric) f='Egalizator și analizor acustic în cascadă' ;;
        inst.desktopComment) f='EQ parametric, convolutor, crossfeed și limitator true peak pentru PipeWire cu analizor în cascadă 3D în timp real' ;;
        inst.menuOk) f='În meniu și pe desktop: Aqulify' ;;
        inst.step6) f='[6/6] Ieșire audio implicită' ;;
        inst.defaultExplain) f='Sunetul aplicațiilor este redat prin Aqulify atunci când „Aqulify” este setat ca dispozitiv de ieșire.' ;;
        inst.legacyDefault) f='Anterior, AetherAudio era ieșirea implicită: Aqulify preia controlul.' ;;
        inst.askDefault) f='Doriți să setați ca ieșire implicită acum? [d/N] ' ;;
        inst.defaultOk) f='„Aqulify” este acum ieșirea audio implicită.' ;;
        inst.defaultFail) f='Setarea a eșuat; selectați din setările de sunet (Ieșire: Aqulify).' ;;
        inst.defaultLater) f='Îl puteți selecta mai târziu în setările de sunet (Ieșire: Aqulify).' ;;
        inst.done) f='Gata! Aqulify a fost instalat cu succes.' ;;
        inst.sumGui) f='Interfață:      Meniu → Aqulify  (sau: aqulify-gui)' ;;
        inst.sumState) f='Stare motor:    systemctl --user status aqulify' ;;
        inst.sumLog) f='Jurnal:         journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Dezinstalare:   ./uninstall.sh' ;;
        esac ;;
    el) case "$2" in
        common.errorPrefix) f='[Σφάλμα]' ;;
        inst.title) f='Aqulify – εγκατάσταση στο Linux' ;;
        inst.noRoot) f='Μην το εκτελείτε ως root! Ξεκινήστε το ως απλός χρήστης (θα ζητηθεί ο κωδικός πρόσβασης sudo για τα πακέτα).' ;;
        inst.step1) f='[1/6] Εγκατάσταση απαραίτητων πακέτων...' ;;
        inst.noOsRelease) f='Δεν είναι δυνατός ο προσδιορισμός της διανομής (/etc/os-release).' ;;
        inst.unknownDistro1) f='Άγνωστη διανομή (%s). Απαιτούνται: μεταγλωττιστής C/C++20, cmake, pkg-config, πακέτα ανάπτυξης libpipewire-0.3 και fftw3 (float)·' ;;
        inst.unknownDistro2) f='για το παράθυρο της εφαρμογής: πακέτα ανάπτυξης gtk3 και webkit2gtk-4.1 (ή 4.0).' ;;
        inst.noWebkit) f='Το πακέτο WebKitGTK δεν εγκαταστάθηκε: η διεπαφή θα ανοίξει στο πρόγραμμα περιήγησης.' ;;
        inst.noPwCli) f='Το pw-cli δεν βρέθηκε. Είναι πράγματι το PipeWire το σύστημα ήχου σας;' ;;
        inst.systemPkg) f='Είναι επίσης εγκατεστημένο ένα πακέτο Aqulify (/usr/bin). Αυτή η εγκατάσταση (~/.local) έχει προτεραιότητα· για να χρησιμοποιήσετε την έκδοση πακέτου, εκτελέστε το ./uninstall.sh αργότερα.' ;;
        inst.step2) f='[2/6] Μεταγλώττιση...' ;;
        inst.rnnSystem) f='RNNoise (μείωση θορύβου φωνής): πακέτο συστήματος.' ;;
        inst.rnnDownload) f='Λήψη μοντέλου RNNoise (αρχείο Ubuntu, περίπου 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (μείωση θορύβου φωνής): ενσωματωμένο.' ;;
        inst.rnnFail) f='Αποτυχία λήψης μοντέλου RNNoise: η μείωση θορύβου φωνής χρησιμοποιεί την ενσωματωμένη μέθοδο ανά ζώνη.' ;;
        inst.engineFail) f='Η μεταγλώττιση του κινητήρα απέτυχε.' ;;
        inst.engineOk) f='Κινητήρας: build/aqulify-engine' ;;
        inst.guiOk) f='Διεπαφή παραθύρου εφαρμογής: build/aqulify-gui' ;;
        inst.guiFail1) f='Δεν ήταν δυνατή η μεταγλώττιση της διεπαφής παραθύρου εφαρμογής (WebKitGTK) (λεπτομέρειες: build/gui-build.log).' ;;
        inst.guiFail2) f='Εναλλακτική λύση: παράθυρο εφαρμογής προγράμματος περιήγησης βασισμένου στο Chromium ή το προεπιλεγμένο πρόγραμμα περιήγησης.' ;;
        inst.guiFail3) f='Λύση: εγκαταστήστε τα πακέτα ανάπτυξης gtk3 και webkit2gtk-4.1 και εκτελέστε ξανά το ./install.sh.' ;;
        inst.step3) f='[3/6] Αντιγραφή αρχείων (%s)...' ;;
        inst.legacy) f='Κατάργηση προηγούμενης εγκατάστασης AetherAudio (οι ρυθμίσεις διατηρούνται)...' ;;
        inst.filesOk) f='Τα αρχεία αντιγράφηκαν επιτυχώς.' ;;
        inst.step4) f='[4/6] Ρύθμιση αυτόματης εκκίνησης (υπηρεσία χρήστη systemd)...' ;;
        inst.unitDesc) f='Κινητήρας DSP Aqulify (εικονική έξοδος ήχου "Aqulify")' ;;
        inst.serviceOk) f='Ο κινητήρας εκτελείται και θα ξεκινά αυτόματα κατά τη σύνδεση.' ;;
        inst.serviceFail) f='Η υπηρεσία χρήστη systemd δεν είναι διαθέσιμη. Εκκινήστε χειροκίνητα: %s &' ;;
        inst.step5) f='[5/6] Καταχώριση μενού...' ;;
        inst.desktopGeneric) f='Ισοσταθμιστής και ακουστικός αναλυτής καταρράκτη' ;;
        inst.desktopComment) f='Παραμετρικό EQ, συνελικτής, crossfeed και περιοριστής true peak για PipeWire με ζωντανό αναλυτή καταρράκτη 3D' ;;
        inst.menuOk) f='Στο μενού και στην επιφάνεια εργασίας: Aqulify' ;;
        inst.step6) f='[6/6] Προεπιλεγμένη έξοδος ήχου' ;;
        inst.defaultExplain) f='Ο ήχος των εφαρμογών αναπαράγεται μέσω του Aqulify όταν το "Aqulify" έχει οριστεί ως συσκευή εξόδου.' ;;
        inst.legacyDefault) f='Προηγουμένως το AetherAudio ήταν η προεπιλογή: το Aqulify αναλαμβάνει.' ;;
        inst.askDefault) f='Ορισμός ως προεπιλεγμένη έξοδος τώρα; [ν/Ο] ' ;;
        inst.defaultOk) f='Το "Aqulify" είναι πλέον η προεπιλεγμένη έξοδος ήχου.' ;;
        inst.defaultFail) f='Αποτυχία ορισμού· επιλέξτε το στις ρυθμίσεις ήχου (Έξοδος: Aqulify).' ;;
        inst.defaultLater) f='Μπορείτε να το επιλέξετε αργότερα στις ρυθμίσεις ήχου (Έξοδος: Aqulify).' ;;
        inst.done) f='Έτοιμο! Το Aqulify εγκαταστάθηκε με επιτυχία.' ;;
        inst.sumGui) f='Διεπαφή:        Μενού → Aqulify  (ή: aqulify-gui)' ;;
        inst.sumState) f='Κατάσταση:      systemctl --user status aqulify' ;;
        inst.sumLog) f='Αρχείο καταγραφής: journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Απεγκατάσταση:  ./uninstall.sh' ;;
        esac ;;
    tr) case "$2" in
        common.errorPrefix) f='[Hata]' ;;
        inst.title) f='Aqulify – Linux üzerinde kurulum' ;;
        inst.noRoot) f='Root olarak çalıştırmayın! Kendi kullanıcınız olarak başlatın (paketler için sudo şifresi istenecektir).' ;;
        inst.step1) f='[1/6] Gerekli paketler kuruluyor...' ;;
        inst.noOsRelease) f='Dağıtım belirlenemiyor (/etc/os-release).' ;;
        inst.unknownDistro1) f='Bilinmeyen dağıtım (%s). Gerekli: C/C++20 derleyici, cmake, pkg-config, libpipewire-0.3 ve fftw3 (float) geliştirme paketleri;' ;;
        inst.unknownDistro2) f='uygulama penceresi için: gtk3 ve webkit2gtk-4.1 (veya 4.0) geliştirme paketleri.' ;;
        inst.noWebkit) f='WebKitGTK paketi yüklenmedi: arayüz tarayıcıda açılacak.' ;;
        inst.noPwCli) f='pw-cli bulunamadı. PipeWire gerçekten ses sisteminiz mi?' ;;
        inst.systemPkg) f='Paketlenmiş bir Aqulify da kurulu (/usr/bin). Bu kurulum (~/.local) artık önceliklidir; paketlenmiş sürümü kullanmak için daha sonra ./uninstall.sh çalıştırın.' ;;
        inst.step2) f='[2/6] Derleniyor...' ;;
        inst.rnnSystem) f='RNNoise (konuşma gürültüsü azaltma): sistem paketi.' ;;
        inst.rnnDownload) f='RNNoise modeli indiriliyor (Ubuntu arşivi, yaklaşık 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (konuşma gürültüsü azaltma): yerleşik.' ;;
        inst.rnnFail) f='RNNoise modeli indirilemedi: konuşma gürültüsü azaltma yerleşik bant bazlı yöntemi kullanır.' ;;
        inst.engineFail) f='Motor derlemesi başarısız oldu.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Uygulama penceresi arayüzü: build/aqulify-gui' ;;
        inst.guiFail1) f='Uygulama penceresi arayüzü (WebKitGTK) derlenemedi (ayrıntılar: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternatif: Chromium tabanlı bir tarayıcının uygulama penceresi veya varsayılan web tarayıcısı.' ;;
        inst.guiFail3) f='Çözüm: gtk3 ve webkit2gtk-4.1 geliştirme paketlerini kurun ve ./install.sh komutunu tekrar çalıştırın.' ;;
        inst.step3) f='[3/6] Dosyalar kopyalanıyor (%s)...' ;;
        inst.legacy) f='Önceki AetherAudio kurulumu kaldırılıyor (ayarlar korunur)...' ;;
        inst.filesOk) f='Dosyalar başarıyla kopyalandı.' ;;
        inst.step4) f='[4/6] Otomatik başlatma ayarlanıyor (systemd kullanıcı hizmeti)...' ;;
        inst.unitDesc) f='Aqulify DSP motoru ("Aqulify" sanal ses çıkışı)' ;;
        inst.serviceOk) f='Motor çalışıyor ve oturum açıldığında otomatik olarak başlayacak.' ;;
        inst.serviceFail) f='systemd kullanıcı hizmeti kullanılamıyor. Lütfen manuel başlatın: %s &' ;;
        inst.step5) f='[5/6] Menü girişi...' ;;
        inst.desktopGeneric) f='Ekolayzer ve akustik şelale analizörü' ;;
        inst.desktopComment) f='PipeWire için parametrik EQ, konvolüsyon, crossfeed ve gerçek tepe sınırlayıcı ile gerçek zamanlı 3D şelale analizörü' ;;
        inst.menuOk) f='Menüde ve masaüstünde: Aqulify' ;;
        inst.step6) f='[6/6] Varsayılan ses çıkışı' ;;
        inst.defaultExplain) f='Çıkış aygıtı olarak "Aqulify" seçildiğinde uygulama sesleri Aqulify üzerinden iletilir.' ;;
        inst.legacyDefault) f='Daha önce varsayılan çıkış AetherAudio idi: Aqulify devralır.' ;;
        inst.askDefault) f='Şimdi varsayılan ses çıkışı olarak ayarlansın mı? [e/H] ' ;;
        inst.defaultOk) f='"Aqulify" artık varsayılan ses çıkışıdır.' ;;
        inst.defaultFail) f='Ayarlama başarısız; lütfen ses ayarlarından seçin (Çıkış: Aqulify).' ;;
        inst.defaultLater) f='Bunu daha sonra ses ayarlarından seçebilirsiniz (Çıkış: Aqulify).' ;;
        inst.done) f='Tamamlandı! Aqulify başarıyla kuruldu.' ;;
        inst.sumGui) f='Arayüz:         Menü → Aqulify  (veya: aqulify-gui)' ;;
        inst.sumState) f='Motor durumu:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Günlük:         journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Kaldırma:       ./uninstall.sh' ;;
        esac ;;
    fi) case "$2" in
        common.errorPrefix) f='[Virhe]' ;;
        inst.title) f='Aqulify – asennus Linuxiin' ;;
        inst.noRoot) f='Älä suorita pääkäyttäjänä (root)! Käynnistä tavallisena käyttäjänä (paketeille kysytään sudo-salasanaa).' ;;
        inst.step1) f='[1/6] Asennetaan tarvittavat paketit...' ;;
        inst.noOsRelease) f='Jakelua ei voida määrittää (/etc/os-release).' ;;
        inst.unknownDistro1) f='Tuntematon jakelu (%s). Tarvitaan: C/C++20-kääntäjä, cmake, pkg-config, kehityspaketit libpipewire-0.3 ja fftw3 (float);' ;;
        inst.unknownDistro2) f='sovellusikkunaa varten: gtk3 ja webkit2gtk-4.1 (tai 4.0) -kehityspaketit.' ;;
        inst.noWebkit) f='WebKitGTK-pakettia ei asennettu: käyttöliittymä avataan selaimessa.' ;;
        inst.noPwCli) f='Komentoa pw-cli ei löytynyt. Onko PipeWire varmasti äänijärjestelmäsi?' ;;
        inst.systemPkg) f='Myös paketoitu Aqulify on asennettuna (/usr/bin). Tämä asennus (~/.local) on ensisijainen; käyttääksesi paketoitua versiota suorita myöhemmin ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Käännetään...' ;;
        inst.rnnSystem) f='RNNoise (puheen kohinanvaimennus): järjestelmäpaketti.' ;;
        inst.rnnDownload) f='Ladataan RNNoise-malli (Ubuntu-arkisto, n. 19 Mt)...' ;;
        inst.rnnOk) f='RNNoise (puheen kohinanvaimennus): integroitu.' ;;
        inst.rnnFail) f='RNNoise-mallin lataus epäonnistui: puheen kohinanvaimennus käyttää sisäänrakennettua kaistakohtaista menetelmää.' ;;
        inst.engineFail) f='Moottorin kääntäminen epäonnistui.' ;;
        inst.engineOk) f='Moottori: build/aqulify-engine' ;;
        inst.guiOk) f='Sovellusikkunan käyttöliittymä: build/aqulify-gui' ;;
        inst.guiFail1) f='Sovellusikkunan käyttöliittymää (WebKitGTK) ei voitu kääntää (lisätietoja: build/gui-build.log).' ;;
        inst.guiFail2) f='Vaihtoehto: Chromium-pohjaisen selaimen sovellusikkuna tai oletusselain.' ;;
        inst.guiFail3) f='Ratkaisu: asenna kehityspaketit gtk3 ja webkit2gtk-4.1 ja suorita ./install.sh uudelleen.' ;;
        inst.step3) f='[3/6] Kopioidaan tiedostoja (%s)...' ;;
        inst.legacy) f='Poistetaan aiempi AetherAudio-asennus (asetukset säilytetään)...' ;;
        inst.filesOk) f='Tiedostot kopioitu onnistuneesti.' ;;
        inst.step4) f='[4/6] Määritetään automaattinen käynnistys (systemd-käyttäjäpalvelu)...' ;;
        inst.unitDesc) f='Aqulify DSP -moottori (virtuaalinen äänilähtö "Aqulify")' ;;
        inst.serviceOk) f='Moottori on käynnissä ja käynnistyy automaattisesti sisäänkirjautumisen yhteydessä.' ;;
        inst.serviceFail) f='systemd-käyttäjäpalvelu ei ole käytettävissä. Käynnistä manuaalisesti: %s &' ;;
        inst.step5) f='[5/6] Valikkokohta...' ;;
        inst.desktopGeneric) f='Taajuuskorjain ja akustinen vesiputousanalysaattori' ;;
        inst.desktopComment) f='Parametrinen EQ, konvolveri, crossfeed ja true peak -rajoitin PipeWirelle reaaliaikaisella 3D-vesiputousanalysaattorilla' ;;
        inst.menuOk) f='Valikossa ja työpöydällä: Aqulify' ;;
        inst.step6) f='[6/6] Oletusäänilähtö' ;;
        inst.defaultExplain) f='Sovellusten ääni toistetaan Aqulifyn kautta, kun "Aqulify" on asetettu äänilähdöksi.' ;;
        inst.legacyDefault) f='Aiemmin AetherAudio oli oletuslähtö: Aqulify ottaa ohjat.' ;;
        inst.askDefault) f='Asetetaanko oletusäänilähdöksi nyt? [k/E] ' ;;
        inst.defaultOk) f='"Aqulify" on nyt oletusäänilähtö.' ;;
        inst.defaultFail) f='Asettaminen epäonnistui; valitse se ääniasetuksista (Lähtö: Aqulify).' ;;
        inst.defaultLater) f='Voit valita sen myöhemmin ääniasetuksista (Lähtö: Aqulify).' ;;
        inst.done) f='Valmis! Aqulify on asennettu onnistuneesti.' ;;
        inst.sumGui) f='Käyttöliittymä: Valikko → Aqulify  (tai: aqulify-gui)' ;;
        inst.sumState) f='Moottorin tila: systemctl --user status aqulify' ;;
        inst.sumLog) f='Loki:           journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Asennuksen poisto: ./uninstall.sh' ;;
        esac ;;
    da) case "$2" in
        common.errorPrefix) f='[Fejl]' ;;
        inst.title) f='Aqulify – installation på Linux' ;;
        inst.noRoot) f='Kør ikke som root! Start som almindelig bruger (du bliver bedt om sudo-adgangskode til pakker).' ;;
        inst.step1) f='[1/6] Installerer nødvendige pakker...' ;;
        inst.noOsRelease) f='Kan ikke identificere distributionen (/etc/os-release).' ;;
        inst.unknownDistro1) f='Ukendt distribution (%s). Påkrævet: C/C++20-compiler, cmake, pkg-config, libpipewire-0.3 og fftw3 (float) udviklingspakker;' ;;
        inst.unknownDistro2) f='til applikationsvinduet: gtk3 og webkit2gtk-4.1 (eller 4.0) udviklingspakker.' ;;
        inst.noWebkit) f='WebKitGTK-pakken blev ikke installeret: brugerfladen åbnes i browseren.' ;;
        inst.noPwCli) f='pw-cli blev ikke fundet. Er PipeWire virkelig dit lydsystem?' ;;
        inst.systemPkg) f='En pakket Aqulify er også installeret (/usr/bin). Denne installation (~/.local) har forrang; kør ./uninstall.sh senere for at bruge den pakkede version.' ;;
        inst.step2) f='[2/6] Kompilerer...' ;;
        inst.rnnSystem) f='RNNoise (stemmestøjreduktion): systempakke.' ;;
        inst.rnnDownload) f='Downloader RNNoise-model (Ubuntu-arkiv, ca. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (stemmestøjreduktion): indbygget.' ;;
        inst.rnnFail) f='Download af RNNoise-model mislykkedes: stemmestøjreduktion bruger den indbyggede båndmetode.' ;;
        inst.engineFail) f='Kompilering af motor mislykkedes.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Vinduesgrænseflade: build/aqulify-gui' ;;
        inst.guiFail1) f='Vinduesgrænsefladen (WebKitGTK) kunne ikke kompileres (detaljer: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativ: app-vindue i en Chromium-baseret browser eller standardbrowseren.' ;;
        inst.guiFail3) f='Løsning: Installer gtk3 og webkit2gtk-4.1 udviklingspakker og kør ./install.sh igen.' ;;
        inst.step3) f='[3/6] Kopierer filer (%s)...' ;;
        inst.legacy) f='Fjerner tidligere AetherAudio-installation (indstillinger bevares)...' ;;
        inst.filesOk) f='Filer kopieret.' ;;
        inst.step4) f='[4/6] Opsætter autostart (systemd-bruger-tjeneste)...' ;;
        inst.unitDesc) f='Aqulify DSP-motor ("Aqulify" virtuel lydudgang)' ;;
        inst.serviceOk) f='Motoren kører og starter automatisk ved login.' ;;
        inst.serviceFail) f='systemd-brugertjeneste er ikke tilgængelig. Start manuelt: %s &' ;;
        inst.step5) f='[5/6] Menupunkt...' ;;
        inst.desktopGeneric) f='Equalizer og akustisk vandfaldsanalysator' ;;
        inst.desktopComment) f='Parametrisk EQ, konvolver, crossfeed og true peak-limiter til PipeWire med realtids 3D-vandfaldsanalysator' ;;
        inst.menuOk) f='I menuen og på skrivebordet: Aqulify' ;;
        inst.step6) f='[6/6] Standard lydudgang' ;;
        inst.defaultExplain) f='Applikationslyd afspilles via Aqulify, når "Aqulify" er valgt som outputenhed.' ;;
        inst.legacyDefault) f='Tidligere var AetherAudio standard: Aqulify overtager.' ;;
        inst.askDefault) f='Indstil som standard lydudgang nu? [j/N] ' ;;
        inst.defaultOk) f='"Aqulify" er nu standard lydudgang.' ;;
        inst.defaultFail) f='Indstilling mislykkedes; vælg det i lydindstillingerne (Udgang: Aqulify).' ;;
        inst.defaultLater) f='Du kan vælge det senere i lydindstillingerne (Udgang: Aqulify).' ;;
        inst.done) f='Færdig! Aqulify er installeret korrekt.' ;;
        inst.sumGui) f='Brugerflade:   Menu → Aqulify  (eller: aqulify-gui)' ;;
        inst.sumState) f='Motorstatus:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Logs:          journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Afinstallation: ./uninstall.sh' ;;
        esac ;;
    nb) case "$2" in
        common.errorPrefix) f='[Feil]' ;;
        inst.title) f='Aqulify – installasjon på Linux' ;;
        inst.noRoot) f='Ikke kjør som root! Start som vanlig bruker (du blir bedt om sudo-passord for pakker).' ;;
        inst.step1) f='[1/6] Installerer nødvendige pakker...' ;;
        inst.noOsRelease) f='Kan ikke identifisere distribusjonen (/etc/os-release).' ;;
        inst.unknownDistro1) f='Ukjent distribusjon (%s). Nødvendig: C/C++20-kompilator, cmake, pkg-config, libpipewire-0.3 og fftw3 (float) utviklingspakker;' ;;
        inst.unknownDistro2) f='for applikasjonsvinduet: gtk3 og webkit2gtk-4.1 (eller 4.0) utviklingspakker.' ;;
        inst.noWebkit) f='WebKitGTK-pakken ble ikke installert: brukergrensesnittet åpnes i nettleseren.' ;;
        inst.noPwCli) f='pw-cli ble ikke funnet. Er PipeWire virkelig lydsystemet ditt?' ;;
        inst.systemPkg) f='En pakket Aqulify er også installert (/usr/bin). Denne installasjonen (~/.local) har nå forrang; kjør ./uninstall.sh senere for å bruke den pakkede versjonen.' ;;
        inst.step2) f='[2/6] Kompilerer...' ;;
        inst.rnnSystem) f='RNNoise (stemmestøyreduksjon): systempakke.' ;;
        inst.rnnDownload) f='Laster ned RNNoise-modell (Ubuntu-arkiv, ca. 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (stemmestøyreduksjon): innebygd.' ;;
        inst.rnnFail) f='Nedlasting av RNNoise-modell mislyktes: stemmestøyreduksjon bruker den innebygde båndmetoden.' ;;
        inst.engineFail) f='Kompilering av motor mislyktes.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Vindusgrensesnitt: build/aqulify-gui' ;;
        inst.guiFail1) f='Vindusgrensesnittet (WebKitGTK) kunne ikke kompileres (detaljer: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativ: app-vindu i en Chromium-basert nettleser eller standardnettleseren.' ;;
        inst.guiFail3) f='Løsning: Installer gtk3 og webkit2gtk-4.1 utviklingspakker og kjør ./install.sh på nytt.' ;;
        inst.step3) f='[3/6] Kopierer filer (%s)...' ;;
        inst.legacy) f='Fjerner tidligere AetherAudio-installasjon (innstillinger beholdes)...' ;;
        inst.filesOk) f='Filer kopiert.' ;;
        inst.step4) f='[4/6] Konfigurerer automatisk oppstart (systemd-brukertjeneste)...' ;;
        inst.unitDesc) f='Aqulify DSP-motor ("Aqulify" virtuell lydutgang)' ;;
        inst.serviceOk) f='Motoren kjører og starter automatisk ved innlogging.' ;;
        inst.serviceFail) f='systemd-brukertjeneste er ikke tilgjengelig. Start manuelt: %s &' ;;
        inst.step5) f='[5/6] Menyoppføring...' ;;
        inst.desktopGeneric) f='Equalizer og akustisk fossanalysator' ;;
        inst.desktopComment) f='Parametrisk EQ, konvolver, crossfeed og true peak-limiter for PipeWire med 3D-fossanalysator i sanntid' ;;
        inst.menuOk) f='I menyen og på skrivebordet: Aqulify' ;;
        inst.step6) f='[6/6] Standard lydutgang' ;;
        inst.defaultExplain) f='Applikasjonslyd spilles av via Aqulify når "Aqulify" er valgt som utgangsenhet.' ;;
        inst.legacyDefault) f='Tidligere var AetherAudio standard: Aqulify overtar.' ;;
        inst.askDefault) f='Sett som standard lydutgang nå? [j/N] ' ;;
        inst.defaultOk) f='"Aqulify" er nå standard lydutgang.' ;;
        inst.defaultFail) f='Innstilling mislyktes; velg den i lydinnstillingene (Utgang: Aqulify).' ;;
        inst.defaultLater) f='Du kan velge den senere i lydinnstillingene (Utgang: Aqulify).' ;;
        inst.done) f='Ferdig! Aqulify er installert.' ;;
        inst.sumGui) f='Grensesnitt:   Meny → Aqulify  (eller: aqulify-gui)' ;;
        inst.sumState) f='Motorstatus:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Logger:        journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Avinstallasjon: ./uninstall.sh' ;;
        esac ;;
    sv) case "$2" in
        common.errorPrefix) f='[Fel]' ;;
        inst.title) f='Aqulify – installation på Linux' ;;
        inst.noRoot) f='Kör inte som root! Starta som vanlig användare (du blir ombedd att ange sudo-lösenord för paket).' ;;
        inst.step1) f='[1/6] Installerar nödvändiga paket...' ;;
        inst.noOsRelease) f='Kan inte identifiera distributionen (/etc/os-release).' ;;
        inst.unknownDistro1) f='Okänd distribution (%s). Krävs: C/C++20-kompilator, cmake, pkg-config, utvecklingspaket för libpipewire-0.3 och fftw3 (float);' ;;
        inst.unknownDistro2) f='för programfönstret: utvecklingspaket för gtk3 och webkit2gtk-4.1 (eller 4.0).' ;;
        inst.noWebkit) f='WebKitGTK-paketet installerades inte: gränssnittet öppnas i webbläsaren.' ;;
        inst.noPwCli) f='pw-cli hittades inte. Är PipeWire verkligen ditt ljudsystem?' ;;
        inst.systemPkg) f='Ett paketerat Aqulify är också installerat (/usr/bin). Denna installation (~/.local) har företräde; kör ./uninstall.sh senare för att använda den paketerade versionen.' ;;
        inst.step2) f='[2/6] Kompilerar...' ;;
        inst.rnnSystem) f='RNNoise (röstbrusreducering): systempaket.' ;;
        inst.rnnDownload) f='Laddar ner RNNoise-modell (Ubuntu-arkiv, ca 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (röstbrusreducering): inbyggd.' ;;
        inst.rnnFail) f='Nedladdning av RNNoise-modellen misslyckades: röstbrusreducering använder den inbyggda bandmetoden.' ;;
        inst.engineFail) f='Kompilering av motorn misslyckades.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Programfönstergränssnitt: build/aqulify-gui' ;;
        inst.guiFail1) f='Programfönstergränssnittet (WebKitGTK) kunde inte kompileras (detaljer: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativ: app-fönster i en Chromium-baserad webbläsare eller standardwebbläsaren.' ;;
        inst.guiFail3) f='Lösning: Installera utvecklingspaketen gtk3 och webkit2gtk-4.1 och kör ./install.sh igen.' ;;
        inst.step3) f='[3/6] Kopierar filer (%s)...' ;;
        inst.legacy) f='Tar bort tidigare AetherAudio-installation (inställningar bevaras)...' ;;
        inst.filesOk) f='Filer kopierade.' ;;
        inst.step4) f='[4/6] Konfigurerar autostart (systemd-användartjänst)...' ;;
        inst.unitDesc) f='Aqulify DSP-motor ("Aqulify" virtuell ljudutgång)' ;;
        inst.serviceOk) f='Motorn körs och startar automatiskt vid inloggning.' ;;
        inst.serviceFail) f='systemd-användartjänst är inte tillgänglig. Starta manuellt: %s &' ;;
        inst.step5) f='[5/6] Menypost...' ;;
        inst.desktopGeneric) f='Equalizer och akustisk vattenfallsanalysator' ;;
        inst.desktopComment) f='Parametrisk EQ, faltning, crossfeed och true peak-limiter för PipeWire med 3D-vattenfallsanalysator i realtid' ;;
        inst.menuOk) f='I menyn och på skrivbordet: Aqulify' ;;
        inst.step6) f='[6/6] Standard ljudutgång' ;;
        inst.defaultExplain) f='Programljud spelas via Aqulify när "Aqulify" är vald som utgångsenhet.' ;;
        inst.legacyDefault) f='Tidigare var AetherAudio standard: Aqulify tar över.' ;;
        inst.askDefault) f='Ange som standard ljudutgång nu? [j/N] ' ;;
        inst.defaultOk) f='"Aqulify" är nu standard ljudutgång.' ;;
        inst.defaultFail) f='Inställningen misslyckades; välj den i ljudinställningarna (Utgång: Aqulify).' ;;
        inst.defaultLater) f='Du kan välja den senare i ljudinställningarna (Utgång: Aqulify).' ;;
        inst.done) f='Klart! Aqulify har installerats.' ;;
        inst.sumGui) f='Gränssnitt:    Meny → Aqulify  (eller: aqulify-gui)' ;;
        inst.sumState) f='Motorstatus:   systemctl --user status aqulify' ;;
        inst.sumLog) f='Loggar:        journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Avinstallation: ./uninstall.sh' ;;
        esac ;;
    he) case "$2" in
        common.errorPrefix) f='[שגיאה]' ;;
        inst.title) f='Aqulify – התקנה בלינוקס' ;;
        inst.noRoot) f='אין להפעיל כמשתמש root! הפעל כמשתמש רגיל (תתבקש להזין סיסמת sudo עבור החבילות).' ;;
        inst.step1) f='[1/6] מתקין חבילות נדרשות...' ;;
        inst.noOsRelease) f='לא ניתן לזהות את ההפצה (/etc/os-release).' ;;
        inst.unknownDistro1) f='הפצה לא מוכרת (%s). נדרש: מהדר C/C++20, cmake, pkg-config, חבילות פיתוח של libpipewire-0.3 ו-fftw3 (float);' ;;
        inst.unknownDistro2) f='עבור חלון היישום: חבילות פיתוח gtk3 ו-webkit2gtk-4.1 (או 4.0).' ;;
        inst.noWebkit) f='חבילת WebKitGTK לא הותקנה: הממשק ייפתח בדפדפן.' ;;
        inst.noPwCli) f='pw-cli לא נמצא. האם PipeWire היא אכן מערכת השמע שלך?' ;;
        inst.systemPkg) f='מותקנת גם חבילת Aqulify מהפצה (/usr/bin). התקנה זו (~/.local) מקבלת עדיפות; כדי להשתמש בגרסת החבילה, הפעל את ./uninstall.sh מאוחר יותר.' ;;
        inst.step2) f='[2/6] מהדר...' ;;
        inst.rnnSystem) f='RNNoise (הפחתת רעשי קול): חבילת מערכת.' ;;
        inst.rnnDownload) f='מוריד מודל RNNoise (ארכיון Ubuntu, כ-19 MB)...' ;;
        inst.rnnOk) f='RNNoise (הפחתת רעשי קול): מובנה.' ;;
        inst.rnnFail) f='הורדת מודל RNNoise נכשלה: הפחתת רעשי קול תשתמש בשיטה המובנית לפי תדרים.' ;;
        inst.engineFail) f='הידור המנוע נכשל.' ;;
        inst.engineOk) f='מנוע: build/aqulify-engine' ;;
        inst.guiOk) f='ממשק חלון היישום: build/aqulify-gui' ;;
        inst.guiFail1) f='לא ניתן היה להדר את ממשק חלון היישום (WebKitGTK) (פרטים: build/gui-build.log).' ;;
        inst.guiFail2) f='חלופה: חלון יישום של דפדפן מבוסס Chromium או דפדפן האינטרנט המוגדר כברירת מחדל.' ;;
        inst.guiFail3) f='פתרון: התקן את חבילות הפיתוח gtk3 ו-webkit2gtk-4.1 והפעל שוב את ./install.sh.' ;;
        inst.step3) f='[3/6] מעתיק קבצים (%s)...' ;;
        inst.legacy) f='מסיר התקנה קודמת של AetherAudio (ההגדרות נשמרות)...' ;;
        inst.filesOk) f='הקבצים הועתקו בהצלחה.' ;;
        inst.step4) f='[4/6] מגדיר הפעלה אוטומטית (שירות משתמש של systemd)...' ;;
        inst.unitDesc) f='מנוע DSP של Aqulify (פלט שמע וירטואלי "Aqulify")' ;;
        inst.serviceOk) f='המנוע פועל ויופעל אוטומטית בעת ההתחברות.' ;;
        inst.serviceFail) f='שירות משתמש של systemd אינו זמין. אנא הפעל ידנית: %s &' ;;
        inst.step5) f='[5/6] פריט תפריט...' ;;
        inst.desktopGeneric) f='אקולייזר ומנתח אקוסטיקה במפל מים' ;;
        inst.desktopComment) f='EQ פרמטרי, קונבולבר, crossfeed ולימיטר true peak עבור PipeWire עם מנתח מפל תלת-ממדי בזמן אמת' ;;
        inst.menuOk) f='בתפריט ובשולחן העבודה: Aqulify' ;;
        inst.step6) f='[6/6] פלט שמע ברירת מחדל' ;;
        inst.defaultExplain) f='שמע של יישומים מושמע דרך Aqulify כאשר "Aqulify" מוגדר כהתקן הפלט.' ;;
        inst.legacyDefault) f='בעבר AetherAudio היה ברירת המחדל: Aqulify תופס את מקומו.' ;;
        inst.askDefault) f='האם להגדיר כפלט שמע ברירת מחדל כעת? [y/N] ' ;;
        inst.defaultOk) f='"Aqulify" הוא כעת פלט השמע המוגדר כברירת מחדל.' ;;
        inst.defaultFail) f='ההגדרה נכשלה; אנא בחר בהגדרות השמע (פלט: Aqulify).' ;;
        inst.defaultLater) f='תוכל לבחור זאת מאוחר יותר בהגדרות השמע (פלט: Aqulify).' ;;
        inst.done) f='הסתיים! Aqulify הותקן בהצלחה.' ;;
        inst.sumGui) f='ממשק:          תפריט → Aqulify  (או: aqulify-gui)' ;;
        inst.sumState) f='מצב מנוע:      systemctl --user status aqulify' ;;
        inst.sumLog) f='יומנים:        journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='הסרת התקנה:   ./uninstall.sh' ;;
        esac ;;
    ja) case "$2" in
        common.errorPrefix) f='[エラー]' ;;
        inst.title) f='Aqulify – Linuxへのインストール' ;;
        inst.noRoot) f='rootとして実行しないでください！通常ユーザーとして起動してください（パッケージのインストール時にsudoパスワードが求められます）。' ;;
        inst.step1) f='[1/6] 必要なパッケージをインストールしています...' ;;
        inst.noOsRelease) f='ディストリビューションを識別できません (/etc/os-release)。' ;;
        inst.unknownDistro1) f='未知のディストリビューションです (%s)。必要: C/C++20コンパイラ、cmake、pkg-config、libpipewire-0.3およびfftw3 (float) の開発パッケージ;' ;;
        inst.unknownDistro2) f='アプリケーションウィンドウ用: gtk3およびwebkit2gtk-4.1 (または4.0) の開発パッケージ。' ;;
        inst.noWebkit) f='WebKitGTKパッケージがインストールされませんでした: UIはブラウザで開きます。' ;;
        inst.noPwCli) f='pw-cliが見つかりません。PipeWireは本当にオーディオシステムとして動作していますか？' ;;
        inst.systemPkg) f='パッケージ版のAqulifyもインストールされています (/usr/bin)。このインストール (~/.local) が優先されます。パッケージ版を使用するには、後で ./uninstall.sh を実行してください。' ;;
        inst.step2) f='[2/6] コンパイル中...' ;;
        inst.rnnSystem) f='RNNoise (音声ノイズ低減): システムパッケージ。' ;;
        inst.rnnDownload) f='RNNoiseモデルをダウンロードしています (Ubuntuアーカイブ、約19 MB)...' ;;
        inst.rnnOk) f='RNNoise (音声ノイズ低減): 内蔵。' ;;
        inst.rnnFail) f='RNNoiseモデルのダウンロードに失敗しました: 音声ノイズ低減には内蔵の帯域方式が使用されます。' ;;
        inst.engineFail) f='エンジンのコンパイルに失敗しました。' ;;
        inst.engineOk) f='エンジン: build/aqulify-engine' ;;
        inst.guiOk) f='ウィンドウUI: build/aqulify-gui' ;;
        inst.guiFail1) f='ウィンドウUI (WebKitGTK) をコンパイルできませんでした (詳細: build/gui-build.log)。' ;;
        inst.guiFail2) f='代替案: Chromiumベースのブラウザのアプリウィンドウ、またはデフォルトブラウザ。' ;;
        inst.guiFail3) f='解決策: gtk3およびwebkit2gtk-4.1開発パッケージをインストールして、再度 ./install.sh を実行してください。' ;;
        inst.step3) f='[3/6] ファイルをコピーしています (%s)...' ;;
        inst.legacy) f='以前のAetherAudioインストールを削除しています（設定は保持されます）...' ;;
        inst.filesOk) f='ファイルのコピーが完了しました。' ;;
        inst.step4) f='[4/6] 自動起動を設定しています (systemdユーザーサービス)...' ;;
        inst.unitDesc) f='Aqulify DSPエンジン（仮想オーディオ出力 "Aqulify"）' ;;
        inst.serviceOk) f='エンジンは実行中であり、ログイン時に自動起動します。' ;;
        inst.serviceFail) f='systemdユーザーサービスが利用できません。手動で起動してください: %s &' ;;
        inst.step5) f='[5/6] メニューエントリ...' ;;
        inst.desktopGeneric) f='イコライザー＆音響ウォーターフォールアナライザー' ;;
        inst.desktopComment) f='リアルタイム3Dウォーターフォールアナライザーを備えた、PipeWire用のパラメトリックEQ、コンボルバー、クロスフィード、True Peakリミッター' ;;
        inst.menuOk) f='メニューおよびデスクトップ: Aqulify' ;;
        inst.step6) f='[6/6] デフォルトのオーディオ出力' ;;
        inst.defaultExplain) f='出力デバイスとして「Aqulify」が設定されている場合、アプリのオーディオはAqulify経由で再生されます。' ;;
        inst.legacyDefault) f='以前はAetherAudioがデフォルトでした: Aqulifyが引き継ぎます。' ;;
        inst.askDefault) f='今すぐデフォルトのオーディオ出力として設定しますか？ [y/N] ' ;;
        inst.defaultOk) f='「Aqulify」がデフォルトのオーディオ出力に設定されました。' ;;
        inst.defaultFail) f='設定に失敗しました。サウンド設定から選択してください（出力: Aqulify）。' ;;
        inst.defaultLater) f='後でサウンド設定から選択することもできます（出力: Aqulify）。' ;;
        inst.done) f='完了しました！Aqulifyのインストールに成功しました。' ;;
        inst.sumGui) f='UI:            メニュー → Aqulify  (または: aqulify-gui)' ;;
        inst.sumState) f='状態:          systemctl --user status aqulify' ;;
        inst.sumLog) f='ログ:          journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='アンインストール: ./uninstall.sh' ;;
        esac ;;
    ko) case "$2" in
        common.errorPrefix) f='[오류]' ;;
        inst.title) f='Aqulify – Linux 설치' ;;
        inst.noRoot) f='root 권한으로 실행하지 마십시오! 일반 사용자로 실행해 주십시오 (패키지 설치 시 sudo 비밀번호를 묻습니다).' ;;
        inst.step1) f='[1/6] 필수 패키지 설치 중...' ;;
        inst.noOsRelease) f='배포판을 식별할 수 없습니다 (/etc/os-release).' ;;
        inst.unknownDistro1) f='알 수 없는 배포판입니다 (%s). 필요 항목: C/C++20 컴파일러, cmake, pkg-config, libpipewire-0.3 및 fftw3 (float) 개발 패키지;' ;;
        inst.unknownDistro2) f='애플리케이션 창을 위해: gtk3 및 webkit2gtk-4.1 (또는 4.0) 개발 패키지.' ;;
        inst.noWebkit) f='WebKitGTK 패키지가 설치되지 않았습니다: 브라우저에서 UI가 열립니다.' ;;
        inst.noPwCli) f='pw-cli를 찾을 수 없습니다. 오디오 시스템으로 PipeWire를 사용하고 계십니까?' ;;
        inst.systemPkg) f='패키지된 Aqulify도 설치되어 있습니다 (/usr/bin). 이 설치본(~/.local)이 우선권을 가집니다. 패키지 버전을 사용하려면 나중에 ./uninstall.sh 를 실행하십시오.' ;;
        inst.step2) f='[2/6] 컴파일 중...' ;;
        inst.rnnSystem) f='RNNoise (음성 노이즈 감소): 시스템 패키지.' ;;
        inst.rnnDownload) f='RNNoise 모델 다운로드 중 (Ubuntu 아카이브, 약 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (음성 노이즈 감소): 내장됨.' ;;
        inst.rnnFail) f='RNNoise 모델 다운로드 실패: 음성 노이즈 감소에 내장 대역 방식이 사용됩니다.' ;;
        inst.engineFail) f='엔진 컴파일 실패.' ;;
        inst.engineOk) f='엔진: build/aqulify-engine' ;;
        inst.guiOk) f='창 인터페이스: build/aqulify-gui' ;;
        inst.guiFail1) f='창 인터페이스(WebKitGTK)를 컴파일할 수 없습니다 (자세한 내용: build/gui-build.log).' ;;
        inst.guiFail2) f='대체 방법: Chromium 기반 브라우저의 앱 창 또는 기본 웹 브라우저.' ;;
        inst.guiFail3) f='해결 방법: gtk3 및 webkit2gtk-4.1 개발 패키지를 설치한 후 ./install.sh 를 다시 실행하십시오.' ;;
        inst.step3) f='[3/6] 파일 복사 중 (%s)...' ;;
        inst.legacy) f='이전 AetherAudio 설치본을 제거하는 중 (설정은 유지됨)...' ;;
        inst.filesOk) f='파일 복사 완료.' ;;
        inst.step4) f='[4/6] 자동 시작 설정 중 (systemd 사용자 서비스)...' ;;
        inst.unitDesc) f='Aqulify DSP 엔진 ("Aqulify" 가상 오디오 출력)' ;;
        inst.serviceOk) f='엔진이 실행 중이며 로그인 시 자동으로 시작됩니다.' ;;
        inst.serviceFail) f='systemd 사용자 서비스를 사용할 수 없습니다. 수동으로 시작하십시오: %s &' ;;
        inst.step5) f='[5/6] 메뉴 항목...' ;;
        inst.desktopGeneric) f='이퀄라이저 및 음향 폭포 분석기' ;;
        inst.desktopComment) f='실시간 3D 폭포 분석기를 갖춘 PipeWire용 파라메트릭 EQ, 컨볼버, 크로스피드 및 True Peak 리미터' ;;
        inst.menuOk) f='메뉴 및 바탕화면: Aqulify' ;;
        inst.step6) f='[6/6] 기본 오디오 출력' ;;
        inst.defaultExplain) f='출력 장치로 "Aqulify"가 설정되어 있을 때 앱의 오디오가 Aqulify를 통해 재생됩니다.' ;;
        inst.legacyDefault) f='이전에는 AetherAudio가 기본값이었습니다: 이제 Aqulify가 이어받습니다.' ;;
        inst.askDefault) f='지금 기본 오디오 출력으로 설정하시겠습니까? [y/N] ' ;;
        inst.defaultOk) f='"Aqulify"가 이제 기본 오디오 출력입니다.' ;;
        inst.defaultFail) f='설정에 실패했습니다. 사운드 설정에서 선택해 주십시오 (출력: Aqulify).' ;;
        inst.defaultLater) f='나중에 사운드 설정에서 선택할 수 있습니다 (출력: Aqulify).' ;;
        inst.done) f='완료되었습니다! Aqulify가 성공적으로 설치되었습니다.' ;;
        inst.sumGui) f='인터페이스:     메뉴 → Aqulify  (또는: aqulify-gui)' ;;
        inst.sumState) f='엔진 상태:     systemctl --user status aqulify' ;;
        inst.sumLog) f='로그:          journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='제거:          ./uninstall.sh' ;;
        esac ;;
    cs) case "$2" in
        common.errorPrefix) f='[Chyba]' ;;
        inst.title) f='Aqulify – instalace na Linux' ;;
        inst.noRoot) f='Nespouštějte jako root! Spusťte jako běžný uživatel (pro instalaci balíčků budete vyzváni k zadání hesla sudo).' ;;
        inst.step1) f='[1/6] Instalace potřebných balíčků...' ;;
        inst.noOsRelease) f='Nelze určit distribuci (/etc/os-release).' ;;
        inst.unknownDistro1) f='Neznámá distribuce (%s). Požadováno: C/C++20 kompilátor, cmake, pkg-config, vývojové balíčky libpipewire-0.3 a fftw3 (float);' ;;
        inst.unknownDistro2) f='pro aplikační okno: vývojové balíčky gtk3 a webkit2gtk-4.1 (nebo 4.0).' ;;
        inst.noWebkit) f='Balíček WebKitGTK nebyl nainstalován: rozhraní se otevře v prohlížeči.' ;;
        inst.noPwCli) f='pw-cli nebyl nalezen. Používáte skutečně PipeWire jako zvukový systém?' ;;
        inst.systemPkg) f='V systému je nainstalována také balíčková verze Aqulify (/usr/bin). Tato instalace (~/.local) má nyní přednost; pro použití balíčkové verze spusťte později ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Kompilace...' ;;
        inst.rnnSystem) f='RNNoise (redukce šumu řeči): systémový balíček.' ;;
        inst.rnnDownload) f='Stahování modelu RNNoise (archiv Ubuntu, cca 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (redukce šumu řeči): integrováno.' ;;
        inst.rnnFail) f='Stažení modelu RNNoise selhalo: redukce šumu řeči použije vestavěnou pásmovou metodu.' ;;
        inst.engineFail) f='Kompilace motoru selhala.' ;;
        inst.engineOk) f='Motor: build/aqulify-engine' ;;
        inst.guiOk) f='Uživatelské rozhraní okna: build/aqulify-gui' ;;
        inst.guiFail1) f='Uživatelské rozhraní okna (WebKitGTK) se nepodařilo zkompilovat (podrobnosti: build/gui-build.log).' ;;
        inst.guiFail2) f='Alternativa: okno aplikace v prohlížeči založeném na Chromiu nebo výchozí prohlížeč.' ;;
        inst.guiFail3) f='Řešení: Nainstalujte vývojové balíčky gtk3 a webkit2gtk-4.1 a spusťte ./install.sh znovu.' ;;
        inst.step3) f='[3/6] Kopírování souborů (%s)...' ;;
        inst.legacy) f='Odstraňování předchozí instalace AetherAudio (nastavení bude zachováno)...' ;;
        inst.filesOk) f='Soubory byly zkopírovány.' ;;
        inst.step4) f='[4/6] Nastavení automatického spouštění (uživatelská služba systemd)...' ;;
        inst.unitDesc) f='DSP motor Aqulify (virtuální zvukový výstup "Aqulify")' ;;
        inst.serviceOk) f='Motor běží a spustí se automaticky při přihlášení.' ;;
        inst.serviceFail) f='Uživatelská služba systemd není dostupná. Spusťte ručně: %s &' ;;
        inst.step5) f='[5/6] Položka nabídky...' ;;
        inst.desktopGeneric) f='Ekvalizér a akustický vodopádový analyzátor' ;;
        inst.desktopComment) f='Parametrický EQ, konvoluce, crossfeed a true peak limiter pro PipeWire s 3D vodopádovým analyzátorem v reálném čase' ;;
        inst.menuOk) f='V nabídce a na ploše: Aqulify' ;;
        inst.step6) f='[6/6] Výchozí zvukový výstup' ;;
        inst.defaultExplain) f='Zvuk z aplikací prochází přes Aqulify, když je jako výstupní zařízení nastaveno "Aqulify".' ;;
        inst.legacyDefault) f='Dříve bylo výchozím zařízením AetherAudio: Aqulify jej nahrazuje.' ;;
        inst.askDefault) f='Nastavit nyní jako výchozí zvukový výstup? [a/N] ' ;;
        inst.defaultOk) f='"Aqulify" je nyní výchozím zvukovým výstupem.' ;;
        inst.defaultFail) f='Nastavení selhalo; zvolte jej v Nastavení zvuku (Výstup: Aqulify).' ;;
        inst.defaultLater) f='Můžete jej zvolit později v Nastavení zvuku (Výstup: Aqulify).' ;;
        inst.done) f='Hotovo! Aqulify je úspěšně nainstalováno.' ;;
        inst.sumGui) f='Rozhraní:      Nabídka → Aqulify  (nebo: aqulify-gui)' ;;
        inst.sumState) f='Stav motoru:    systemctl --user status aqulify' ;;
        inst.sumLog) f='Protokoly:      journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Odinstalace:    ./uninstall.sh' ;;
        esac ;;
    uk) case "$2" in
        common.errorPrefix) f='[Помилка]' ;;
        inst.title) f='Aqulify – встановлення на Linux' ;;
        inst.noRoot) f='Не запускайте від імені root! Запустіть від звичайного користувача (для пакетів знадобиться пароль sudo).' ;;
        inst.step1) f='[1/6] Встановлення необхідних пакетів...' ;;
        inst.noOsRelease) f='Неможливо визначити дистрибутив (/etc/os-release).' ;;
        inst.unknownDistro1) f='Невідомий дистрибутив (%s). Потрібні: компілятор C/C++20, cmake, pkg-config, пакети розробки libpipewire-0.3 та fftw3 (float);' ;;
        inst.unknownDistro2) f='для вікна програми: пакети розробки gtk3 та webkit2gtk-4.1 (або 4.0).' ;;
        inst.noWebkit) f='Пакет WebKitGTK не встановлено: інтерфейс відкриється у веб-переглядачі.' ;;
        inst.noPwCli) f='pw-cli не знайдено. Чи дійсно PipeWire є вашою звуковою системою?' ;;
        inst.systemPkg) f='Також знайдено пакетну версію Aqulify (/usr/bin). Ця інсталяція (~/.local) тепер має пріоритет; щоб використовувати версію з пакета, виконайте пізніше ./uninstall.sh.' ;;
        inst.step2) f='[2/6] Компіляція...' ;;
        inst.rnnSystem) f='RNNoise (зменшення шуму мовлення): системний пакет.' ;;
        inst.rnnDownload) f='Завантаження моделі RNNoise (архів Ubuntu, прибл. 19 МБ)...' ;;
        inst.rnnOk) f='RNNoise (зменшення шуму мовлення): інтегровано.' ;;
        inst.rnnFail) f='Не вдалося завантажити модель RNNoise: придушення шуму мовлення працюватиме за вбудованим смуговим методом.' ;;
        inst.engineFail) f='Помилка компіляції двигуна.' ;;
        inst.engineOk) f='Двигун: build/aqulify-engine' ;;
        inst.guiOk) f='Віконний інтерфейс: build/aqulify-gui' ;;
        inst.guiFail1) f='Не вдалося скомпілювати віконний інтерфейс (WebKitGTK) (деталі: build/gui-build.log).' ;;
        inst.guiFail2) f='Альтернатива: вікно програми у браузері на базі Chromium або типовий веб-переглядач.' ;;
        inst.guiFail3) f='Рішення: Встановіть пакети розробки gtk3 та webkit2gtk-4.1, потім знову запустіть ./install.sh.' ;;
        inst.step3) f='[3/6] Копіювання файлів (%s)...' ;;
        inst.legacy) f='Видалення попередньої версії AetherAudio (налаштування зберігаються)...' ;;
        inst.filesOk) f='Файли скопійовано.' ;;
        inst.step4) f='[4/6] Налаштування автозапуску (користувацька служба systemd)...' ;;
        inst.unitDesc) f='DSP-двигун Aqulify (віртуальний аудіовихід "Aqulify")' ;;
        inst.serviceOk) f='Двигун працює і запускатиметься автоматично під час входу в систему.' ;;
        inst.serviceFail) f='Користувацька служба systemd недоступна. Запустіть вручну: %s &' ;;
        inst.step5) f='[5/6] Пункт меню...' ;;
        inst.desktopGeneric) f='Еквалайзер та акустичний водоспадний аналізатор' ;;
        inst.desktopComment) f='Параметричний EQ, конволюція, crossfeed та true peak лімітер для PipeWire з 3D-аналізатором водоспаду в реальному часі' ;;
        inst.menuOk) f='У меню та на робочому столі: Aqulify' ;;
        inst.step6) f='[6/6] Типовий аудіовихід' ;;
        inst.defaultExplain) f='Звук програм проходитиме через Aqulify, якщо вибрано "Aqulify" як вихідний пристрій.' ;;
        inst.legacyDefault) f='Раніше типовим виходом був AetherAudio: Aqulify переймає налаштування.' ;;
        inst.askDefault) f='Встановити як типовий аудіовихід зараз? [т/Н] ' ;;
        inst.defaultOk) f='"Aqulify" тепер є типовим аудіовиходом.' ;;
        inst.defaultFail) f='Не вдалося встановити; виберіть його у Налаштуваннях звуку (Вихід: Aqulify).' ;;
        inst.defaultLater) f='Ви можете вибрати його пізніше у Налаштуваннях звуку (Вихід: Aqulify).' ;;
        inst.done) f='Готово! Aqulify успішно встановлено.' ;;
        inst.sumGui) f='Інтерфейс:     Меню → Aqulify  (або: aqulify-gui)' ;;
        inst.sumState) f='Стан двигуна:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Журнали:       journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Видалення:     ./uninstall.sh' ;;
        esac ;;
    esac
    if [ -z "$f" ]; then
        case "$2" in
        common.errorPrefix) f='[Error]' ;;
        inst.title) f='Aqulify – installation on Linux' ;;
        inst.noRoot) f='Do not run it as root! Start it as your own user (it will ask for the sudo password for the packages).' ;;
        inst.step1) f='[1/6] Installing required packages...' ;;
        inst.noOsRelease) f='Cannot determine the distribution (/etc/os-release).' ;;
        inst.unknownDistro1) f='Unknown distribution (%s). Required: C/C++20 compiler, cmake, pkg-config, libpipewire-0.3 and fftw3 (float) development packages;' ;;
        inst.unknownDistro2) f='for the app window: gtk3 and webkit2gtk-4.1 (or 4.0) development packages.' ;;
        inst.noWebkit) f='The WebKitGTK package was not installed: the interface will open in a browser.' ;;
        inst.noPwCli) f='pw-cli was not found. Is your sound system really PipeWire?' ;;
        inst.systemPkg) f='A packaged Aqulify is also installed (/usr/bin). This installation (~/.local) takes precedence from now on; to use the packaged one instead, run ./uninstall.sh afterwards.' ;;
        inst.step2) f='[2/6] Building...' ;;
        inst.rnnSystem) f='RNNoise (speech noise reduction): system package.' ;;
        inst.rnnDownload) f='Downloading the RNNoise model (Ubuntu archive, about 19 MB)...' ;;
        inst.rnnOk) f='RNNoise (speech noise reduction): built in.' ;;
        inst.rnnFail) f='The RNNoise model could not be downloaded: speech noise reduction uses the built-in band-wise method.' ;;
        inst.engineFail) f='Building the engine failed.' ;;
        inst.engineOk) f='Engine: build/aqulify-engine' ;;
        inst.guiOk) f='App window interface: build/aqulify-gui' ;;
        inst.guiFail1) f='The app window (WebKitGTK) interface did not build (details: build/gui-build.log).' ;;
        inst.guiFail2) f='Fallback: the app window of a Chromium-based browser, or if there is none, the default browser.' ;;
        inst.guiFail3) f='Fix: install the gtk3 and webkit2gtk-4.1 development packages, then run ./install.sh again.' ;;
        inst.step3) f='[3/6] Copying files (%s)...' ;;
        inst.legacy) f='Removing the previous AetherAudio installation (the settings are carried over)...' ;;
        inst.filesOk) f='Files in place.' ;;
        inst.step4) f='[4/6] Setting up automatic start (systemd user service)...' ;;
        inst.unitDesc) f='Aqulify DSP engine (virtual "Aqulify" audio output)' ;;
        inst.serviceOk) f='The engine is running and starts automatically at login.' ;;
        inst.serviceFail) f='The systemd user service is not available. Start it manually: %s &' ;;
        inst.step5) f='[5/6] Menu entry...' ;;
        inst.desktopGeneric) f='Equalizer and acoustic waterfall analyzer' ;;
        inst.desktopComment) f='Parametric EQ, convolver, crossfeed and true peak limiter for PipeWire with a live 3D waterfall analyzer' ;;
        inst.menuOk) f='In the menu and on the desktop: Aqulify' ;;
        inst.step6) f='[6/6] Default audio output' ;;
        inst.defaultExplain) f='Application audio goes through Aqulify when "Aqulify" is the output device.' ;;
        inst.legacyDefault) f='The previous AetherAudio was the default output: Aqulify takes over.' ;;
        inst.askDefault) f='Set it as the default now? [y/N] ' ;;
        inst.defaultOk) f='"Aqulify" is now the default output.' ;;
        inst.defaultFail) f='That did not work; set it in the Sound settings (Output: Aqulify).' ;;
        inst.defaultLater) f='You can select it later in the Sound settings (Output: Aqulify).' ;;
        inst.done) f='Done! Aqulify is installed.' ;;
        inst.sumGui) f='Interface:      menu → Aqulify  (or: aqulify-gui)' ;;
        inst.sumState) f='Engine status:  systemctl --user status aqulify' ;;
        inst.sumLog) f='Log:            journalctl --user -u aqulify -f' ;;
        inst.sumUninstall) f='Uninstall:      ./uninstall.sh' ;;
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
# desktop_l10n MEZŐ KULCS – a .desktop fájl fordított sorai (MEZŐ[nyelv]=...)
desktop_l10n() {
    local l
    for l in hu es fr zh de it nl pl ru pt ro el tr fi da nb sv he ja ko cs uk; do
        printf "%s[%s]=%s\n" "$1" "$l" "$(msg_text "$l" "$2")"
    done
}
# <<< i18n

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
say()  { echo -e "${YELLOW}$*${NC}"; }
ok()   { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
die()  { echo -e "${RED}$(msg common.errorPrefix) $*${NC}"; exit 1; }

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

PREFIX="${HOME}/.local"
BIN_DIR="${PREFIX}/bin"
SHARE_DIR="${PREFIX}/share/aqulify"
GUI_DIR="${SHARE_DIR}/gui"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
APP_DIR="${PREFIX}/share/applications"
ICON_DIR="${PREFIX}/share/icons/hicolor/256x256/apps"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${GREEN}  $(msg inst.title)${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$(id -u)" -eq 0 ] && [ -z "${AQULIFY_ALLOW_ROOT:-}" ] && die "$(msg inst.noRoot)"

# ------------------------------------------------------------------ 1. csomagok
say "\n$(msg inst.step1)"
. /etc/os-release 2>/dev/null || die "$(msg inst.noOsRelease)"
ID_ALL="${ID:-} ${ID_LIKE:-}"
GUI_PKGS_OK=1
case "$ID_ALL" in
    *ubuntu*|*debian*)
        sudo apt-get update
        sudo apt-get install -y build-essential cmake pkg-config libpipewire-0.3-dev libfftw3-dev pipewire
        # RNNoise (a Magic Touch beszéd zajcsökkentése): újabb kiadásokban csomagként is van
        sudo apt-get install -y librnnoise-dev 2>/dev/null || true
        sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.1-dev \
            || sudo apt-get install -y libgtk-3-dev libwebkit2gtk-4.0-dev || GUI_PKGS_OK=0
        ;;
    *fedora*|*rhel*|*centos*)
        sudo dnf install -y gcc gcc-c++ cmake pkgconf pipewire-devel fftw-devel pipewire
        sudo dnf install -y gtk3-devel webkit2gtk4.1-devel || GUI_PKGS_OK=0
        ;;
    *arch*|*manjaro*|*endeavouros*)
        sudo pacman -S --needed --noconfirm base-devel cmake pkgconf pipewire fftw
        sudo pacman -S --needed --noconfirm gtk3 webkit2gtk-4.1 || GUI_PKGS_OK=0
        ;;
    *suse*)
        sudo zypper install -y gcc gcc-c++ cmake pkg-config pipewire-devel fftw3-devel pipewire
        sudo zypper install -y gtk3-devel webkit2gtk3-devel || GUI_PKGS_OK=0
        ;;
    *)
        warn "$(msg inst.unknownDistro1 "${ID:-?}")"
        warn "$(msg inst.unknownDistro2)"
        ;;
esac
[ "$GUI_PKGS_OK" = 1 ] || warn "$(msg inst.noWebkit)"

command -v pw-cli >/dev/null 2>&1 || warn "$(msg inst.noPwCli)"
# Két telepítés egyszerre (T28): a ~/.config/systemd/user szolgáltatás felülírja a csomagét, így a ~/.local motor indul
if [ -x /usr/bin/aqulify-engine ]; then warn "$(msg inst.systemPkg)"; fi

# ------------------------------------------------------------------ 2. fordítás
say "\n$(msg inst.step2)"
# RNNoise: a rendszer csomagja, ha van; különben a betanított modell az Ubuntu forráscsomagjából (ellenőrzött sha256)
RNN_OPT=""
if pkg-config --exists rnnoise 2>/dev/null; then
    ok "$(msg inst.rnnSystem)"
else
    RNN_DIR="$PROJECT_DIR/build/rnnoise-model"
    if [ ! -f "$RNN_DIR/rnnoise_data.c" ]; then
        say "$(msg inst.rnnDownload)"
        mkdir -p "$RNN_DIR"
        RNN_TAR="$RNN_DIR/rnnoise.orig.tar.xz"
        bash "$PROJECT_DIR/tools/fetch_rnnoise.sh" "$RNN_TAR" >/dev/null 2>&1 &&
            tar -xJf "$RNN_TAR" -C "$RNN_DIR" --strip-components=2 rnnoise-0.2/src/rnnoise_data.c rnnoise-0.2/src/rnnoise_data.h 2>/dev/null
        rm -f "$RNN_TAR"
    fi
    if [ -f "$RNN_DIR/rnnoise_data.c" ] && [ -f "$RNN_DIR/rnnoise_data.h" ]; then
        RNN_OPT="-DAQULIFY_RNNOISE_MODEL_DIR=$RNN_DIR"
        ok "$(msg inst.rnnOk)"
    else
        warn "$(msg inst.rnnFail)"
    fi
fi
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON -DBUILD_LINUX_GUI=ON $RNN_OPT
cmake --build build -j"$(nproc)" --target aqulify-engine aqulify-play
[ -x build/aqulify-engine ] || die "$(msg inst.engineFail)"
ok "$(msg inst.engineOk)"
HAVE_GUI=0
if cmake --build build -j"$(nproc)" --target aqulify-gui > build/gui-build.log 2>&1 && [ -x build/aqulify-gui ]; then
    HAVE_GUI=1
    ok "$(msg inst.guiOk)"
else
    tail -n 15 build/gui-build.log 2>/dev/null || true
    warn "$(msg inst.guiFail1)"
    warn "$(msg inst.guiFail2)"
    warn "$(msg inst.guiFail3)"
fi

# ------------------------------------------------------------------ 3. fájlok
say "\n$(msg inst.step3 "${PREFIX}")"
# Futó motor leállítása frissítéskor
systemctl --user stop aqulify.service >/dev/null 2>&1 || true
# Régi név (AetherAudio, 1.4.x-ig) eltávolítása: szolgáltatás, programfájlok, menü, ikon.
# A beállítások és profilok a ~/.config/aetheraudio mappából az első indításkor átköltöznek.
OLD="aether""audio"
if [ -e "$UNIT_DIR/${OLD}.service" ] || [ -e "$BIN_DIR/${OLD}-engine" ]; then
    say "$(msg inst.legacy)"
    WAS_DEFAULT=0
    pw-metadata 0 default.configured.audio.sink 2>/dev/null | grep -q "\"${OLD}_sink\"" && WAS_DEFAULT=1
    systemctl --user disable --now "${OLD}.service" >/dev/null 2>&1 || true
    rm -f "$UNIT_DIR/${OLD}.service" "$BIN_DIR/${OLD}-engine" "$BIN_DIR/${OLD}-gui"
    rm -rf "${PREFIX}/share/${OLD}"
    rm -f "$APP_DIR/${OLD}.desktop" "$ICON_DIR/${OLD}.png" "${PREFIX}/share/pixmaps/${OLD}.png"
    for d in "$HOME/Asztal" "$HOME/Desktop"; do rm -f "$d/${OLD}.desktop"; done
    systemctl --user daemon-reload >/dev/null 2>&1 || true
fi
PIXMAP_DIR="${PREFIX}/share/pixmaps"
mkdir -p "$BIN_DIR" "$GUI_DIR" "$APP_DIR" "$ICON_DIR" "$UNIT_DIR" "$PIXMAP_DIR"
install -m 755 build/aqulify-engine "$BIN_DIR/aqulify-engine"
# 2.x: a beépített lejátszó parancssori indítója (aqulify-play <fájl>)
if [ -x build/aqulify-play ]; then install -m 755 build/aqulify-play "$BIN_DIR/aqulify-play"; fi
install -m 644 gui_integrated_system.html autoeq_index.js gui_i18n.js gui_player.js "$GUI_DIR/"
install -m 644 windows/third_party/three/three.min.js "$GUI_DIR/three.min.js"
install -m 644 windows/third_party/three/LICENSE "$GUI_DIR/three.js-LICENSE.txt"
install -m 644 linux/aqulify.png "$ICON_DIR/aqulify.png"
install -m 644 linux/aqulify.png "$PIXMAP_DIR/aqulify.png"
if [ "$HAVE_GUI" = 1 ]; then
    install -m 755 build/aqulify-gui "$BIN_DIR/aqulify-gui"
else
    cat > "$BIN_DIR/aqulify-gui" <<EOF
#!/usr/bin/env bash
# Aqulify felület WebKitGTK nélkül (a beépített saját ablakhoz telepítsd a webkit2gtk fejlesztői csomagot,
# és futtasd újra az install.sh-t). Chromium alapú böngésző alkalmazásablaka (--app), végső esetben xdg-open.
URL="file://${GUI_DIR}/gui_integrated_system.html"
for b in chromium chromium-browser google-chrome google-chrome-stable brave-browser microsoft-edge vivaldi; do
    if command -v "\$b" >/dev/null 2>&1; then
        exec "\$b" --app="\$URL" --class=Aqulify --window-size=1480,940
    fi
done
exec xdg-open "\$URL"
EOF
    chmod 755 "$BIN_DIR/aqulify-gui"
fi
ok "$(msg inst.filesOk)"

# ------------------------------------------------------------------ 4. automatikus indítás
say "\n$(msg inst.step4)"
cat > "$UNIT_DIR/aqulify.service" <<EOF
[Unit]
Description=$(msg inst.unitDesc)
After=pipewire.service wireplumber.service
Wants=pipewire.service

[Service]
ExecStart=${BIN_DIR}/aqulify-engine
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF
if systemctl --user daemon-reload 2>/dev/null && systemctl --user enable --now aqulify.service; then
    ok "$(msg inst.serviceOk)"
else
    warn "$(msg inst.serviceFail "${BIN_DIR}/aqulify-engine")"
fi

# ------------------------------------------------------------------ 5. menübejegyzés
say "\n$(msg inst.step5)"
# A fájlnév az ablak alkalmazás-azonosítója (aether_gui_linux.cpp: kAppId): Waylanden ez alapján kapja az ablak
# az ikonját. A 2.1.5-ig használt aqulify.desktop nevet lecseréljük.
DESKTOP_ID="io.github.szorgos.Aqulify.desktop"
rm -f "$APP_DIR/aqulify.desktop"
cat > "$APP_DIR/$DESKTOP_ID" <<EOF
[Desktop Entry]
Type=Application
Name=Aqulify
GenericName=$(msg_text en inst.desktopGeneric)
$(desktop_l10n GenericName inst.desktopGeneric)
Comment=$(msg_text en inst.desktopComment)
$(desktop_l10n Comment inst.desktopComment)
Exec=${BIN_DIR}/aqulify-gui
Icon=${ICON_DIR}/aqulify.png
Terminal=false
Categories=AudioVideo;Audio;
StartupWMClass=Aqulify
EOF
chmod +x "$APP_DIR/$DESKTOP_ID"

# Asztali ikon létrehozása (ha létezik Asztal vagy Desktop mappa)
for d in "$HOME/Asztal" "$HOME/Desktop"; do
    if [ -d "$d" ]; then
        cp "$APP_DIR/$DESKTOP_ID" "$d/aqulify.desktop"
        chmod +x "$d/aqulify.desktop"
        gio set "$d/aqulify.desktop" metadata::trusted true >/dev/null 2>&1 || true
    fi
done

update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -q "${PREFIX}/share/icons/hicolor" >/dev/null 2>&1 || true
kbuildsycoca6 >/dev/null 2>&1 || kbuildsycoca5 >/dev/null 2>&1 || true
# régi (0.x) telepítés maradványa
rm -f "$PROJECT_DIR/run.sh" 2>/dev/null || true
ok "$(msg inst.menuOk)"

# ------------------------------------------------------------------ 6. alapértelmezett kimenet
say "\n$(msg inst.step6)"
msg inst.defaultExplain; echo
if [ "${WAS_DEFAULT:-0}" = 1 ]; then
    ans="y"; msg inst.legacyDefault; echo
else
    read -r -p "$(msg inst.askDefault)" ans || ans=""
fi
# Igen: i (igen) vagy y (yes)
case "$ans" in
    [iIyY]*) ans="y" ;;
    *) ans="" ;;
esac
if [ "$ans" = "y" ]; then
    sleep 1
    if pw-metadata 0 default.configured.audio.sink '{ "name": "aqulify_sink" }' Spa:String:JSON >/dev/null 2>&1; then
        ok "$(msg inst.defaultOk)"
    else
        warn "$(msg inst.defaultFail)"
    fi
else
    msg inst.defaultLater; echo
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${GREEN}  $(msg inst.done)${NC}"
echo -e "${BLUE}====================================================================${NC}"
for k in inst.sumGui inst.sumState inst.sumLog inst.sumUninstall; do echo "  $(msg "$k")"; done
