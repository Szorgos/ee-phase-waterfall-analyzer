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
        hu|en|es|fr|zh) printf "%s" "$l" ;;
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
    for l in hu es fr zh; do
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
    RNN_URL="http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz"
    RNN_SHA="db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b"
    RNN_DIR="$PROJECT_DIR/build/rnnoise-model"
    if [ ! -f "$RNN_DIR/rnnoise_data.c" ]; then
        say "$(msg inst.rnnDownload)"
        mkdir -p "$RNN_DIR"
        RNN_TAR="$RNN_DIR/rnnoise.orig.tar.xz"
        { curl -fsSL -o "$RNN_TAR" "$RNN_URL" 2>/dev/null || wget -q -O "$RNN_TAR" "$RNN_URL" 2>/dev/null; } &&
            echo "$RNN_SHA  $RNN_TAR" | sha256sum -c --status &&
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
