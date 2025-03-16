#include "frei0r.hpp"

#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zstd.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <algorithm>
#include <future>
#include <optional>
#include <map>
#include <vector>
#include <vips/vips8>

typedef std::tuple<int, int, bool> maskCacheKey;
typedef std::tuple<int, int, vips::VImage> maskCacheValue;
std::map<maskCacheKey, maskCacheValue> maskCache;
std::list<maskCacheKey> maskCacheLRU;
std::mutex maskCacheMutex;
const int maskCacheCapacity = 500;

// if the mask would end at less than this pixels from an image border, the mask
// is enlarged to avoid a small sliver of "non blurred" at the image edge.
const int minMaskGap = 10;

class Jsonblur : public frei0r::filter {
public:
    Jsonblur(unsigned int width, unsigned int height) {
        m_skipFramesStart = 0;
        m_skipFramesEvery = 0;
        m_jsonPath = "";
        m_minScore = 0.2;
        m_debug = 0.0;
        register_param(m_jsonPath, "jsonPath", "Path to the .json.gz from which to read the anonymizations");
        register_param(
            m_skipFramesStart, "skipFramesStart", "How many frames to ignore from the beginning of the .json.gz");
        register_param(m_minScore,
                       "minScore",
                       "Float from 0.0 to 1.0. The larger, the higher the confidence the detection is correct. By "
                       "default objects with a score greater than 0.2 will be blurred.");
        register_param(m_skipFramesEvery,
                       "skipFramesEvery",
                       "How many frames to skip after every blurred frame. Use with FPS reduction like so: -vsync vfr "
                       "-filter_complex 'select=not(mod(n\\,15)),frei0r=jsonblur:video.MP4.json.gz|0|0.2|15'");
        register_param(m_debug, "debug", "Render frame and detection indexes onto image");
    }

    ~Jsonblur() {}

    virtual void update(double time, uint32_t *out, const uint32_t *in) {
        auto result = as_vips_image(out);
        as_vips_image(in).write(result);

        if (m_debug > 0) draw_text(result, 0, 0, "frame=" + std::to_string((int)m_skipFramesStart), false);

        auto blurs = get_blurs_for_frame();

        for (int i = 0; i < blurs.Size(); i++) {
            auto blur = blurs[i].GetObject();

            double score = blur["score"].GetDouble();
            if (score < m_minScore) continue;

            int x = round(blur["x_min"].GetDouble());
            int y = round(blur["y_min"].GetDouble());
            int w = round(blur["x_max"].GetDouble()) - x;
            int h = round(blur["y_max"].GetDouble()) - y;
            auto kind = blur["kind"].GetString();
            float roundCornerRatio = 0.0;
            if (strcmp("face", kind) == 0) roundCornerRatio = 1.0;
            if (strcmp("person", kind) == 0) roundCornerRatio = 0.8;

            // if detection is at a border, simply enlarge mask to hide rounded
            // corners
            if (roundCornerRatio > 0 && (x + w > width - minMaskGap || x < minMaskGap)) w = w * 2;
            if (roundCornerRatio > 0 && (y + h > height - minMaskGap || y < minMaskGap)) h = h * 2;

            auto [offX, offY, mask] = create_mask(w, h, roundCornerRatio);

            // top/left needs shifting to stay centered
            if (roundCornerRatio > 0 && (x < minMaskGap)) offX = offX + w * 0.5;
            if (roundCornerRatio > 0 && (y < minMaskGap)) offY = offY + h * 0.5;

            // clamp to top left corner
            int left = std::max(0, x - offX);
            int top = std::max(0, y - offY);

            // ensure mask does not overflow original image
            int mLeft = std::min(0, x - offX) * -1;
            int mTop = std::min(0, y - offY) * -1;
            int mWidth = std::min(mask.width(), int(width) - left) - mLeft;
            int mHeight = std::min(mask.height(), int(height) - top) - mTop;
            if (mWidth < 0 || mHeight < 0) continue;
            mask = mask.crop(mLeft, mTop, mWidth, mHeight);

            double blurStrength = round(std::max(4.0, std::max(w, h) / 10.0));
            auto cropped = result.crop(left, top, mask.width(), mask.height());
            auto blurCrop = cropped.gaussblur(blurStrength,
                                              vips::VImage::option()->set("precision", VIPS_PRECISION_APPROXIMATE));
            cropped = mask.ifthenelse(blurCrop, cropped, vips::VImage::option()->set("blend", true));

            result.draw_image(cropped, left, top);

            if (m_debug > 0) draw_text(result, top + mHeight / 2, left + mWidth / 2, std::to_string(i), true);
        }
    }

private:
    std::string m_jsonPath;
    double m_skipFramesStart;
    double m_skipFramesEvery;
    double m_minScore;
    double m_debug;

    rapidjson::Document m_blurs;
    rapidjson::Value::ConstMemberIterator m_blurs_iterator;
    rapidjson::Value::ConstMemberIterator m_blurs_last;

    void draw_text(vips::VImage img, int top, int left, std::string text, bool centered) {
        auto font_size = width / 100;
        auto font = "Sans " + std::to_string(font_size);
        auto raster = vips::VImage::text(text.c_str(), vips::VImage::option()->set("font", font.c_str()));
        if (centered) {
            left -= raster.width() / 2;
            top -= raster.height() / 2;
        }
        img.draw_image(raster, left, top);
    }

    vips::VImage as_vips_image(const uint32_t *location) {
        const int bands = 4;
        const int uints_in_uint32 = 4;

        return vips::VImage::new_from_memory(
            (void *)location, size * uints_in_uint32 * bands, width, height, bands, VipsBandFormat::VIPS_FORMAT_UCHAR);
    }

    const double percentageBoost = 0.5;
    const double blurRadius = 5;
    const int blurMaskModulo = 5;
    maskCacheValue create_mask(int w, int h, float roundCornerRatio) {
        std::lock_guard<std::mutex> guard(maskCacheMutex);

        // round up blur areas to the nearest n pixels to improve cache usage
        w = w + blurMaskModulo - (w % blurMaskModulo);
        h = h + blurMaskModulo - (h % blurMaskModulo);

        auto args = std::make_tuple(w, h, roundCornerRatio);
        auto memoized = maskCache.find(args);
        if (memoized != maskCache.end()) {
            return memoized->second;
        }

        // do not enlarge the blur area too much for small detections
        double bW = std::min(2 * blurRadius, percentageBoost * w);
        double bH = std::min(2 * blurRadius, percentageBoost * h);

        double maskW = w + 2 * bW;
        double maskH = h + 2 * bH;

        int offX = bW + blurRadius;
        int offY = bH + blurRadius;

        double radX = maskW / 2 * roundCornerRatio;
        double radY = maskH / 2 * roundCornerRatio;

        char *svg = g_strdup_printf(
            "<svg viewBox=\"0 0 %g %g\"><rect width=\"%g\" height=\"%g\" x=\"%g\" y=\"%g\" rx=\"%g\" ry=\"%g\" "
            "fill=\"#fff\" /></svg>",
            maskW + 2 * blurRadius,
            maskH + 2 * blurRadius,
            maskW,
            maskH,
            blurRadius,
            blurRadius,
            radX,
            radY);

        auto tinyMask = vips::VImage::new_from_buffer(std::string(svg), "")
                            .extract_band(1)
                            .gaussblur(blurRadius,
                                       vips::VImage::option()->set("precision", VIPS_PRECISION_APPROXIMATE));
        g_free(svg);

        if (maskCache.size() >= maskCacheCapacity) {
            // evict oldest element
            auto i = --maskCacheLRU.end();
            maskCache.erase(*i);
            maskCacheLRU.erase(i);
        }
        maskCacheLRU.push_front(args);
        maskCache[args] = std::make_tuple(offX, offY, tinyMask);

        return maskCache[args];
    }

    bool blurs_loaded = false;
    int retries = 0;
    rapidjson::GenericArray<true, rapidjson::Value> get_blurs_for_frame() {
        if (!blurs_loaded) blurs_loaded = load_blurs_from_disk();

        if (!blurs_loaded || m_blurs_iterator == m_blurs_last) {
            blurs_loaded = false;
            retries++;
            auto wait = std::min(10.0 * 60.0, std::pow(2, retries));

            std::cerr << "WARNING: Trying to blur more frames than we have blur info for (";
            std::cerr << m_jsonPath << "). Currently at frame " << round(m_skipFramesStart);
            std::cerr << ". Waiting " << wait << "s before retry..." << std::endl;

            sleep(wait);
            return get_blurs_for_frame();
        }

        retries = 0;

        auto blurs = m_blurs_iterator->value.GetArray();
        ++m_blurs_iterator;
        m_skipFramesStart += 1.0;
        for (int i = 0; i < m_skipFramesEvery - 1; i++) {
            ++m_blurs_iterator;
            m_skipFramesStart += 1.0;
        }
        return blurs;
    }

    std::vector<std::string> extensions = {
        ".json.gz", ".json.zst", ".json.gz_wip", ".json.zst_wip", ".json_wip", "_wip", ""};

    bool load_blurs_from_disk() {
        std::ifstream file;

        bool isZst = false;
        for (const auto &ext : extensions) {
            auto fullPath = m_jsonPath + ext;
            file.open(fullPath, std::ios_base::in | std::ios_base::binary);
            if (file.is_open()) {
                isZst = fullPath.ends_with(".zst") || fullPath.ends_with(".zst_wip");
                std::cerr << "Loading blurs from " << (m_jsonPath + ext) << std::endl;
                break;
            }
        }

        if (!file.is_open()) {
            std::cerr << "WARNING: JSON blur info not found at: " << m_jsonPath << std::endl;
            return false;
        }

        boost::iostreams::filtering_stream<boost::iostreams::input> decompressor;
        if (isZst) {
            decompressor.push(boost::iostreams::zstd_decompressor());
        } else {
            decompressor.push(boost::iostreams::gzip_decompressor());
        }
        decompressor.push(file);

        rapidjson::IStreamWrapper isw(decompressor);
        m_blurs.ParseStream(isw);

        if (m_blurs.HasParseError()) {
            std::cerr << "WARNING: JSON blur failed to parse: " << m_jsonPath << std::endl;
            return false;
        }
        if (!m_blurs.IsObject()) {
            std::cerr << "WARNING: JSON blur has unexpected format, should have a map at top level: " << m_jsonPath
                      << std::endl;
            return false;
        }

        m_blurs_iterator = m_blurs.MemberBegin();
        m_blurs_last = m_blurs.MemberEnd();

        for (int i = 0; i < round(m_skipFramesStart); i++) m_blurs_iterator++;

        file.close();
        return true;
    }
};

frei0r::construct<Jsonblur> plugin("Jsonblur filter",
                                   "takes detections from an external .json.gz and blurs them in the video",
                                   "Stefan Breunig",
                                   0,
                                   2,
                                   F0R_COLOR_MODEL_RGBA8888);
