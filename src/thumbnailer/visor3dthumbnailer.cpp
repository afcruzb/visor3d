// KIO thumbnail creator for STL, 3MF and STEP files. The policy (embedded 3MF
// preview, else software render) lives in core: v3d::makeThumbnail().

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>
#include <QFile>
#include <QImage>

#include <algorithm>

#include "thumbnail.h"

class Visor3DThumbnailer : public KIO::ThumbnailCreator {
    Q_OBJECT

public:
    Visor3DThumbnailer(QObject* parent, const QVariantList& args) : KIO::ThumbnailCreator(parent, args) {}

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest& request) override {
        const QString local = request.url().toLocalFile();
        if (local.isEmpty()) return KIO::ThumbnailResult::fail();
        const QSize size = request.targetSize();
        const int w = std::clamp(size.width(), 16, 2048), h = std::clamp(size.height(), 16, 2048);

        v3d::LoadOptions options;
        options.allowSlowImport = false;  // never block the KIO worker on a big STEP import
        std::optional<v3d::Thumbnail> thumb = v3d::makeThumbnail(QFile::encodeName(local).toStdString(), w, h, options);
        if (!thumb) return KIO::ThumbnailResult::fail();

        QImage img;
        if (const auto* png = std::get_if<v3d::EncodedPng>(&*thumb)) {
            if (!img.loadFromData(png->bytes.data(), int(png->bytes.size()), "PNG")) return KIO::ThumbnailResult::fail();
        } else {
            const auto& rgba = std::get<v3d::RgbaImage>(*thumb);
            img = QImage(rgba.pixels.data(), rgba.width, rgba.height, rgba.width * 4, QImage::Format_RGBA8888).copy();
        }
        return KIO::ThumbnailResult::pass(img);
    }
};

K_PLUGIN_CLASS_WITH_JSON(Visor3DThumbnailer, "visor3dthumbnailer.json")

#include "visor3dthumbnailer.moc"
