#include "frei0r.hpp"

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <vector>
#include <future>
#include <Magick++.h>

class Jsonblur : public frei0r::filter
{

public:
    Jsonblur(unsigned int width, unsigned int height)
    {
        m_skipFrames = 0;
        m_jsonPath = "";

        register_param(m_jsonPath, "jsonPath", "Path to the .json.gz from which to read the anonymizations");
        register_param(m_skipFrames, "skipFrames", "How many frames to ignore from the beginning of the .json.gz");
    }

    ~Jsonblur() {}

    virtual void update(double time, uint32_t *out, const uint32_t *in)
    {
        auto imgFuture = load_image(in);
        auto blurs = get_blurs_for_frame();

        std::vector<Magick::Geometry> regions;
        regions.reserve(blurs.size());

        auto fullMask = Magick::Image(Magick::Geometry(width, height), Magick::Color("black"));

        for (auto &[_idx, blur] : blurs)
        {
            int x = round(blur.get<double>("x_min"));
            int y = round(blur.get<double>("y_min"));
            int w = round(blur.get<double>("x_max")) - x;
            int h = round(blur.get<double>("y_max")) - y;
            auto kind = blur.get<std::string>("kind");
            float roundCornerRatio = 0.0;
            if (kind == "face")
                roundCornerRatio = 1.0;
            if (kind == "person")
                roundCornerRatio = 0.5;

            auto [tinyMask, region] = create_mask(w, h, roundCornerRatio);
            region.xOff(region.xOff() + x);
            region.yOff(region.yOff() + y);

            regions.push_back(region);

            fullMask.composite(tinyMask, region, Magick::PlusCompositeOp);
        }

        auto img = imgFuture.get();
        fullMask.negate();
        img.mask(fullMask);

        for (const auto &region : regions)
        {
            Magick::Image crop = img;
            crop.crop(region);

            // increase blur strength for big areas to avoid them still being recognizable
            int blurStrength = std::max(5.0, std::max(region.width(), region.height()) / 10.0);
            crop.blur(0, blurStrength);

            img.composite(crop, region, Magick::OverCompositeOp);
        }

        img.write(0, 0, width, height, "RGBA", Magick::StorageType::CharPixel, out);
    }

private:
    std::string m_jsonPath;
    double m_skipFrames;
    Magick::Image m_fullMask;

    boost::property_tree::ptree m_blurs;
    boost::property_tree::ptree::const_iterator m_blurs_iterator;
    boost::property_tree::ptree::const_iterator m_blurs_last;

    std::future<Magick::Image> load_image(const uint32_t *in)
    {
        auto w = width;
        auto h = height;
        return std::async(std::launch::async, [w, h, in]
                          { return Magick::Image(w, h, "RGBA", Magick::StorageType::CharPixel, in); });
    }

    const double percentageBoost = 0.1;
    const double blurRadius = 5;
    const int blurMaskModulo = 5;
    std::map<std::tuple<int, int, bool>, std::tuple<Magick::Image, Magick::Geometry>> maskCache;
    std::tuple<Magick::Image, Magick::Geometry> create_mask(int w, int h, float roundCornerRatio)
    {
        // round up blur areas to the nearest n pixels to improve cache usage
        w = w + blurMaskModulo - (w % blurMaskModulo);
        h = h + blurMaskModulo - (h % blurMaskModulo);

        auto args = std::make_tuple(w, h, roundCornerRatio);
        auto memoized = maskCache.find(args);
        if (memoized != maskCache.end())
        {
            return memoized->second;
        }

        double bW = std::max(2.0 * blurRadius, percentageBoost * w);
        double bH = std::max(2.0 * blurRadius, percentageBoost * h);

        Magick::Geometry region(w + 2 * bW + 4 * blurRadius,
                                h + 2 * bH + 4 * blurRadius,
                                -bW - 2 * blurRadius,
                                -bH - 2 * blurRadius);

        Magick::Image tinyMask(region, Magick::Color("black"));
        tinyMask.fillColor(Magick::Color("white"));
        tinyMask.strokeWidth(0);

        double roundW = bW;
        double roundH = bH;
        roundW += roundCornerRatio * w;
        roundH += roundCornerRatio * h;

        tinyMask.draw(Magick::DrawableRoundRectangle(blurRadius * 2,
                                                     blurRadius * 2,
                                                     w + 2 * bW + blurRadius * 2,
                                                     h + 2 * bH + blurRadius * 2,
                                                     roundW, roundH));
        tinyMask.blur(0, blurRadius);

        maskCache[args] = std::make_tuple(tinyMask, region);
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
            auto wait = std::pow(2, retries);

            std::cerr << "WARNING: Trying to blur more frames than we have blur info for (";
            std::cerr << m_jsonPath << "). Waiting " << wait << "s before retry..." << std::endl;

            sleep(wait);
            return get_blurs_for_frame();
        }

        retries = 0;

        boost::property_tree::ptree blurs = m_blurs_iterator->second;
        ++m_blurs_iterator;
        m_skipFrames += 1.0;
        return blurs;
    }

    bool load_blurs_from_disk()
    {
        std::cerr << "Loading blurs from " << m_jsonPath << std::endl;
        std::ifstream file;
        file.open(m_jsonPath, std::ios_base::in | std::ios_base::binary);
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
