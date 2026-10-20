# WebView2 SDK fejlécek

`WebView2.h` és `EventToken.h`: a Microsoft WebView2 SDK MIDL által generált fejlécei
(Microsoft.Web.WebView2 NuGet csomag, BSD-3-Clause licenc, © Microsoft Corporation).

Forrás ebben a tárolóban: a webui-dev/webui projekt `src/webview/` mappája
(commit efac0e24dd4e6199dd7308c485906c90fb8816a2), mert a NuGet-csomag a fordítókörnyezetből nem érhető el.

A WebView2Loader.dll nem szükséges: a `webview/webview` 0.12.0 beépített betöltője
(`WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL`) közvetlenül a telepített WebView2 futtatókörnyezetet használja.
