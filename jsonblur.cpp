#include "frei0r.hpp"

#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
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

class Jsonblur : public frei0r::filter
{

public:
    Jsonblur(unsigned int width, unsigned int height)
    {
        m_skipFrames = 0;
        m_jsonPath = "";
        m_minScore = 0.2;
        register_param(m_jsonPath, "jsonPath", "Path to the .json.gz from which to read the anonymizations");
        register_param(m_skipFrames, "skipFrames", "How many frames to ignore from the beginning of the .json.gz");
        register_param(m_minScore, "minScore", "Float from 0.0 to 1.0. The larger, the higher the confidence the detection is correct. By default objects with a score greater than 0.2 will be blurred.");
    }

    ~Jsonblur() {}

    virtual void update(double time, uint32_t *out, const uint32_t *in)
    {
        auto result = as_vips_image(out);
        as_vips_image(in).write(result);

        for (auto &[_idx, blur] : get_blurs_for_frame())
        {
            double score = blur.get<double>("score");
            if (score < m_minScore)
                continue;

            int x = round(blur.get<double>("x_min"));
            int y = round(blur.get<double>("y_min"));
            int w = round(blur.get<double>("x_max")) - x;
            int h = round(blur.get<double>("y_max")) - y;
            auto kind = blur.get<std::string>("kind");
            float roundCornerRatio = 0.0;
            if (kind == "face")
                roundCornerRatio = 1.0;
            if (kind == "person")
                roundCornerRatio = 0.8;

            // if detection is at a border, simply enlarge mask to hide rounded
            // corners
            if (roundCornerRatio > 0 && (x + w > width - 10 || x < 10))
                w = w * 2;
            if (roundCornerRatio > 0 && (y + h > height - 10 || y < 10))
                h = h * 2;

            auto [offX, offY, mask] = create_mask(w, h, roundCornerRatio);

            // top/left needs shifting to stay centered
            if (roundCornerRatio > 0 && (x < 5))
                offX = offX + w * 0.5;
            if (roundCornerRatio > 0 && (y < 5))
                offY = offY + h * 0.5;

            // clamp to top left corner
            int left = std::max(0, x - offX);
            int top = std::max(0, y - offY);

            // ensure mask does not overflow original image
            int mLeft = std::min(0, x - offX) * -1;
            int mTop = std::min(0, y - offY) * -1;
            int mWidth = std::min(mask.width(), int(width) - left) - mLeft;
            int mHeight = std::min(mask.height(), int(height) - top) - mTop;
            if (mWidth < 0 || mHeight < 0)
                continue;
            mask = mask.crop(mLeft, mTop, mWidth, mHeight);

            double blurStrength = round(std::max(4.0, std::max(w, h) / 10.0));
            auto cropped = result.crop(left, top, mask.width(), mask.height());
            auto blurCrop = cropped.gaussblur(blurStrength, vips::VImage::option()->set("precision", VIPS_PRECISION_APPROXIMATE));
            cropped = mask.ifthenelse(blurCrop, cropped, vips::VImage::option()->set("blend", true));

            result.draw_image(cropped, left, top);
        }
    }

private:
    std::string m_jsonPath;
    double m_skipFrames;
    double m_minScore;

    boost::property_tree::ptree m_blurs;
    boost::property_tree::ptree::const_iterator m_blurs_iterator;
    boost::property_tree::ptree::const_iterator m_blurs_last;

    vips::VImage as_vips_image(const uint32_t *location)
    {
        const int bands = 4;
        const int uints_in_uint32 = 4;

        return vips::VImage::new_from_memory((void *)location, size * uints_in_uint32 * bands, width, height, bands, VipsBandFormat::VIPS_FORMAT_UCHAR);
    }

    const double percentageBoost = 0.5;
    const double blurRadius = 5;
    const int blurMaskModulo = 5;
    maskCacheValue create_mask(int w, int h, float roundCornerRatio)
    {
        std::lock_guard<std::mutex> guard(maskCacheMutex);

        // round up blur areas to the nearest n pixels to improve cache usage
        w = w + blurMaskModulo - (w % blurMaskModulo);
        h = h + blurMaskModulo - (h % blurMaskModulo);

        auto args = std::make_tuple(w, h, roundCornerRatio);
        auto memoized = maskCache.find(args);
        if (memoized != maskCache.end())
        {
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
            "<svg viewBox=\"0 0 %g %g\"><rect width=\"%g\" height=\"%g\" x=\"%g\" y=\"%g\" rx=\"%g\" ry=\"%g\" fill=\"#fff\" /></svg>",
            maskW + 2 * blurRadius, maskH + 2 * blurRadius,
            maskW, maskH,
            blurRadius, blurRadius,
            radX, radY);

        auto tinyMask = vips::VImage::new_from_buffer(std::string(svg), "")
                            .extract_band(1)
                            .gaussblur(blurRadius, vips::VImage::option()->set("precision", VIPS_PRECISION_APPROXIMATE));

        if (maskCache.size() >= maskCacheCapacity)
        {
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
    boost::property_tree::ptree get_blurs_for_frame()
    {
        if (!blurs_loaded)
            blurs_loaded = load_blurs_from_disk();

        if (!blurs_loaded || m_blurs_iterator == m_blurs_last)
        {
            blurs_loaded = false;
            retries++;
            auto wait = std::min(10.0 * 60.0, std::pow(2, retries));

            std::cerr << "WARNING: Trying to blur more frames than we have blur info for (";
            std::cerr << m_jsonPath << "). Currently at frame " << round(m_skipFrames);
            std::cerr << ". Waiting " << wait << "s before retry..." << std::endl;

            sleep(wait);
            return get_blurs_for_frame();
        }

        retries = 0;

        auto blurs = m_blurs_iterator->second;
        ++m_blurs_iterator;
        m_skipFrames += 1.0;
        return blurs;
    }

    bool load_blurs_from_disk()
    {
        std::cerr << "Loading blurs from " << m_jsonPath << std::endl;
        std::ifstream file;
        file.open(m_jsonPath, std::ios_base::in | std::ios_base::binary);

        // if the completed file doesn't exist, check if there's a WIP one we
        // can use
        if (!file)
            file.open(m_jsonPath + "_wip", std::ios_base::in | std::ios_base::binary);

        if (!file)
        {
            std::cerr << "WARNING: JSON blur info not found at: " << m_jsonPath << std::endl;
            return false;
        }

        boost::iostreams::filtering_stream<boost::iostreams::input> decompressor;
        decompressor.push(boost::iostreams::gzip_decompressor());
        decompressor.push(file);

        boost::property_tree::read_json(decompressor, m_blurs);
        m_blurs_iterator = m_blurs.begin();
        m_blurs_last = m_blurs.end();

        for (int i = 0; i < round(m_skipFrames); i++)
            m_blurs_iterator++;

        file.close();
        return true;
    }
};

frei0r::construct<Jsonblur> plugin("Jsonblur filter",
                                   "takes detections from an external .json.gz and blurs them in the video",
                                   "Stefan Breunig",
                                   0, 2,
                                   F0R_COLOR_MODEL_RGBA8888);
