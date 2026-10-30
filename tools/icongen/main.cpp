// icongen：把 SVG 图标母版按多档像素尺寸光栅化为透明背景 PNG。
// 用法：icongen <输入.svg> <输出目录>
// 输出：<输出目录>/qcnote-<尺寸>.png（16/24/32/48/64/256 六档）

#include <QGuiApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <QSvgRenderer>

#include <cstdio>

namespace {

// 六档输出尺寸（像素）
constexpr int kSizes[] = {16, 24, 32, 48, 64, 256};

// 按指定边长渲染 SVG 到透明画布，并保存为 PNG
bool renderPng(QSvgRenderer &renderer, int size, const QString &outPath)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent); // 透明背景
    // 固定 PNG 的 pHYs DPI 元数据为 100 DPI（3937 dots/meter）：QGuiApplication 会从平台
    // 主题取默认 DPI（随系统缩放/环境变化，曾致 pHYs 从 3937 漂移到 3780、SHA256 不一致），
    // 显式固定以保证生成链字节级可重现（该元数据只影响查看器标称物理尺寸，不影响像素内容）。
    constexpr int kDotsPerMeter = 3937; // 100 DPI
    image.setDotsPerMeterX(kDotsPerMeter);
    image.setDotsPerMeterY(kDotsPerMeter);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, size, size));
    painter.end();

    if (!image.save(outPath, "PNG")) {
        std::fprintf(stderr, "保存失败: %s\n", qPrintable(outPath));
        return false;
    }
    std::printf("已生成: %s (%dx%d)\n", qPrintable(outPath), size, size);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // QGuiApplication（而非 QCoreApplication）：程序使用 QImage/QPainter 等 Gui 绘制类，
    // 未来 SVG 母版加入 text 元素时依赖 Gui 层字体处理，Core 应用下会静默降级且难以排查。
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() != 3) {
        std::fprintf(stderr, "用法: icongen <输入.svg> <输出目录>\n");
        return 1;
    }

    const QString svgPath = args.at(1);
    const QString outDirPath = args.at(2);

    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "SVG 加载失败: %s\n", qPrintable(svgPath));
        return 1;
    }

    QDir outDir(outDirPath);
    if (!outDir.exists() && !outDir.mkpath(QStringLiteral("."))) {
        std::fprintf(stderr, "输出目录创建失败: %s\n", qPrintable(outDirPath));
        return 1;
    }

    for (int size : kSizes) {
        const QString outPath = outDir.filePath(QStringLiteral("qcnote-%1.png").arg(size));
        if (!renderPng(renderer, size, outPath))
            return 1;
    }
    return 0;
}
