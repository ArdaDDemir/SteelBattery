# SteelBattery

Kablosuz SteelSeries cihazlarının pil yüzdesini Windows görev çubuğunda, saatin hemen solunda gösterir.
Tek exe, ek kütüphane yok, ~200 KB'lık bir C++ uygulaması.

```
🎧 ▮▮▮▯ 82% ⚡   🖱 ▮▮▮▯ 75%
```

## Özellikler

- **Görev çubuğunda pil**: özel çizilmiş ikonlar, renkli pil göstergesi (yeşil / turuncu / kırmızı), şarjda ⚡.
- **Detay kartı**: üzerine gelince cihaz adı, durum, bağlantı türü, kalan süre tahmini ve son 24 saatin grafiği.
- **Kalan süre tahmini**: pilin düşme / dolma hızından "~7 sa kaldı", "Dolmasına ~40 dk".
- **Uyku desteği**: fare uykuya geçince son okunan yüzde kalır; uyanınca 5 sn içinde güncellenir.
- **Klavye ekranı**: Apex 7 / Apex Pro OLED ekranında pil durumu (SteelSeries GG üzerinden), şarj animasyonu.
- **Bluetooth**: Windows'un pil bilgisini bildiği bağlı Bluetooth cihazlar (ör. Xbox kumandası).
- **Düşük pil uyarısı**: eşik altında kırmızı yazı ve Windows bildirimi.
- **Kullanım istatistikleri**: her cihaz için bugün / 7 / 30 gün aktif kullanım, uyku ve şarj süresi, şarj sayısı,
  harcanan pil, saatte tüketim ve "tam şarjla ~X saat"; son 14 günün grafiği. Veriler `stats.csv` olarak saklanır (Excel'de açılır).
- **Oyunda sessiz**: tam ekran bir uygulama öndeyken hiçbir pencereye dokunmaz (görev çubuğu, kart, bildirim);
  bildirim oyundan çıkınca gösterilir.
- **Hatalı okuma filtresi**: tek seferlik saçma değerler (ör. %75 iken bir anlık %10) ikinci bir okumayla doğrulanmadan gösterilmez.
- **Ayarlar** (sağ tık): uyarı eşiği, yenileme sıklığı, klavye ekranı, Bluetooth, Windows ile başlat, istatistikler, günlük dosyası.

## Desteklenen cihazlar

| Cihaz | Bağlantı | USB PID |
|---|---|---|
| Arctis Nova 5 / 5X | 2.4 GHz alıcı | 0x2232 / 0x2253 |
| Aerox 5 Wireless (+ Destiny 2 / Diablo IV sürümleri) | kablo ve 2.4 GHz alıcı | 0x1854, 0x185E, 0x1862 / 0x1852, 0x185C, 0x1860 |
| Pil bilgisi veren Bluetooth cihazlar | Bluetooth | — |

Başka SteelSeries modeli eklemek için `src/battery.cpp` içindeki `kSpecs` tablosuna satır eklemek yeterli
(protokol aynıysa).

## Derleme ve kurulum

Gerekenler: Windows 10, Visual Studio 2022+ (veya Build Tools) "C++ ile masaüstü geliştirme".

```bat
build.bat      :: derler ve testleri çalıştırır -> out\SteelBattery.exe
install.bat    :: %LOCALAPPDATA%\Programs\SteelBattery'ye kopyalar, Windows ile başlatır, çalıştırır
uninstall.bat  :: durdurur, başlangıçtan çıkarır, kurulu kopyayı siler
```

Kodu değiştirdikten sonra `build.bat` ve ardından `install.bat` çalıştırmak yeterli.

## Nasıl çalışır

- Pil, cihaza HID üzerinden doğrudan sorulur (VID 0x1038, arayüz 3, 65 baytlık rapor):
  - Aerox 5: komut `0x92` (kablo) / `0xD2` (alıcı). Yanıt: bit7 = şarj, seviye = `((b & 0x7F) - 1) * 5`.
    Alıcıya komut `HidD_SetOutputReport` ile gönderilir, çünkü SteelSeries GG açıkken alıcı
    interrupt yazımlarını yok sayıyor.
  - Arctis Nova 5: komut `0xB0`. Yanıt: `[2] == 0x02` çevrimdışı, `[4]` yüzde, `[5] == 0x01` şarj.
  - Protokoller [rivalcfg](https://github.com/flozz/rivalcfg) ve
    [HeadsetControl](https://github.com/Sapd/HeadsetControl) projelerinden alınmış, gerçek cihazda doğrulanmıştır.
- Görev çubuğu: `Shell_TrayWnd` içine katmanlı (per-pixel alpha) bir alt pencere yerleştirilir,
  görev düğmeleri alanı (`ReBarWindow32`) yer açmak için daraltılır. Explorer yeniden başlarsa kendini yeniden yerleştirir.
- Klavye ekranı: SteelSeries GG'nin yerel GameSense API'sine (`coreProps.json` adresi) 128x40 tek bitlik görüntü gönderilir.
- Ayarlar `HKCU\Software\SteelBattery`; pil geçmişi (`history.txt`), kullanım istatistikleri (`stats.csv`) ve
  tanılama günlüğü (`log.txt`: tutulan şüpheli okumalar, odağı alan pencereler, takılan okumalar)
  `%LOCALAPPDATA%\SteelBattery` içinde tutulur.
- Okuma takılmaya karşı korumalı: cihaza istek zaman aşımlı gönderilir, 20 sn'de sonuç gelmeyen okuma bırakılıp yenisi başlatılır.

## Bilinen sınırlar

- Sadece Windows 10 görev çubuğu (Windows 11'in görev çubuğu farklı çalışır).
- Sadece ana monitördeki görev çubuğu.

## Lisans

[MIT](LICENSE). SteelSeries, Arctis, Aerox ve Apex SteelSeries ApS'nin markalarıdır; bu proje SteelSeries ile
bağlantılı değildir.

## Klasörler

```
src/     uygulama (main, battery = HID/Bluetooth, render = çizim, history, settings, oled)
tests/   çözümleyici ve kalan süre testleri, OLED önizleme aracı
docs/    tasarım ve plan notları
```
