#include "frei0r.hpp"

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

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
        Magick::Image img = Magick::Image(width, height, "RGBA", Magick::StorageType::CharPixel, in);
        boost::property_tree::ptree blurs = get_blurs_for_frame(time);
        for (auto &[_idx, blur] : blurs)
        {
            img = multi_blur_region(img, blur);
        }
        img.write(0, 0, width, height, "RGBA", Magick::StorageType::CharPixel, out);
    }

private:
    std::string m_jsonPath;
    double m_skipFrames;
    boost::property_tree::ptree m_blurs;
    boost::property_tree::ptree::const_iterator m_blurs_iterator;
    boost::property_tree::ptree::const_iterator m_blurs_last;

    boost::property_tree::ptree get_blurs_for_frame(double time)
    {
        load_blurs_from_disk(time);
        if (m_blurs_iterator == m_blurs_last)
        {
            std::cerr << "ERROR: Trying to blur more frames than we have blur info for (";
            std::cerr << m_jsonPath << ")" << std::endl;
            exit(EXIT_FAILURE);
        }

        boost::property_tree::ptree blurs = m_blurs_iterator->second;
        ++m_blurs_iterator;
        return blurs;
    }

    Magick::Image multi_blur_region(Magick::Image img, boost::property_tree::ptree const &blur)
    {
        std::map<std::string, int> dict = to_int_dict(blur);
        int x = dict["x_min"];
        int y = dict["y_min"];
        int w = dict["x_max"] - x;
        int h = dict["y_max"] - y;

        img = single_blur_region(img, w, h, x, y, 15, 0);
        img = single_blur_region(img, w, h, x, y, 4, 4);
        img = single_blur_region(img, w, h, x, y, 2, 8);
        img = single_blur_region(img, w, h, x, y, 1, 12);
        return img;
    }

    Magick::Image single_blur_region(Magick::Image img, int w, int h, int x, int y, int sigma, int extraPixels)
    {
        Magick::Geometry region = Magick::Geometry(w + 2 * extraPixels, h + 2 * extraPixels,
                                                   x - extraPixels, y - extraPixels);

        Magick::Image crop = img;
        crop.crop(region);
        crop.blur(0, sigma);
        img.composite(crop, region, Magick::AtopCompositeOp);
        return img;
    }

    bool blurs_read = false;
    void load_blurs_from_disk(double time)
    {
        if (blurs_read)
            return;
        blurs_read = true;

        std::cerr << "Loading blurs from " << m_jsonPath << std::endl;
        std::ifstream file;
        file.open(m_jsonPath, std::ios_base::in | std::ios_base::binary);
        if (!file)
        {
            std::cerr << "ERROR: JSON blur info not found at: " << m_jsonPath << std::endl;
            exit(EXIT_FAILURE);
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
    }

    std::map<std::string, int> to_int_dict(boost::property_tree::ptree const &pt)
    {
        std::map<std::string, int> dict;
        for (auto &[k, v] : pt)
        {
            if (k == "kind")
                continue;
            dict.emplace(k, round(v.get_value<double>()));
        }
        return dict;
    }
};

frei0r::construct<Jsonblur> plugin("Jsonblur filter",
                                   "takes detections from an external .json.gz and blurs them in the video",
                                   "Stefan Breunig",
                                   0, 2,
                                   F0R_COLOR_MODEL_RGBA8888);
