#!/bin/sh
# Monta Ponte.app a partir do pacote Swift (sem Xcode): compila em release, cria o pacote do app
# com Info.plist, ícone e perfis, e assina localmente (ad hoc).
#
# Uso, na pasta mac/:   ./scripts/build-app.sh
# Resultado:            build/Ponte.app      (arraste para /Applications)
#                       build/Ponte-Mac.zip  (o mesmo app compactado, publicado pelo CI como Ponte-Mac)
set -eu

cd "$(dirname "$0")/.."
VERSION="1.0.1"
APP="build/Ponte.app"

swift build -c release --product PonteMac
BIN="$(swift build -c release --show-bin-path)/PonteMac"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/profiles"
cp "$BIN" "$APP/Contents/MacOS/Ponte"
cp ../profiles/*.json "$APP/Contents/Resources/profiles/"

# ícone: Resources/AppIcon.png (1024x1024) -> AppIcon.icns
ICONSET="build/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z $size $size Resources/AppIcon.png --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z $double $double Resources/AppIcon.png --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Ponte</string>
  <key>CFBundleDisplayName</key><string>Ponte</string>
  <key>CFBundleIdentifier</key><string>io.github.souzaq001.ponte</string>
  <key>CFBundleExecutable</key><string>Ponte</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppSleepDisabled</key><true/>
  <key>NSLocalNetworkUsageDescription</key>
  <string>O Ponte responde aos consoles da sua rede local (PS5 e Switch) para mostrar e copiar a sua biblioteca de jogos.</string>
  <key>NSHumanReadableCopyright</key><string>Uso pessoal, rede local.</string>
</dict>
</plist>
EOF

codesign --force --deep --sign - "$APP"
rm -f build/Ponte.zip build/Ponte-Mac.zip
ditto -c -k --keepParent "$APP" build/Ponte-Mac.zip
echo "Pronto: $(pwd)/$APP"
