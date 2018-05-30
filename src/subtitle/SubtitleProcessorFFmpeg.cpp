/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2018 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV (from 2014)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/private/SubtitleProcessor.h"
#include "QtAV/private/factory.h"
#include "QtAV/AVDemuxer.h"
#include "QtAV/Packet.h"
#include "QtAV/private/AVCompat.h"
#include "utils/internal.h"
#include "PlainText.h"
#include "utils/Logger.h"
#include "VideoFrame.h"

namespace QtAV {

class SubtitleProcessorFFmpeg Q_DECL_FINAL: public SubtitleProcessor
{
public:
    SubtitleProcessorFFmpeg();
    ~SubtitleProcessorFFmpeg();
    SubtitleProcessorId id() const Q_DECL_OVERRIDE;
    QString name() const Q_DECL_OVERRIDE;
    QStringList supportedTypes() const Q_DECL_OVERRIDE;
    bool process(QIODevice* dev) Q_DECL_OVERRIDE;
    // supportsFromFile must be true
    bool process(const QString& path) Q_DECL_OVERRIDE;
    QList<SubtitleFrame> frames() const Q_DECL_OVERRIDE;
    bool processHeader(const QByteArray& codec, const QByteArray& data) Q_DECL_OVERRIDE;
    SubtitleFrame processLine(const QByteArray& data, qreal pts = -1, qreal duration = 0) Q_DECL_OVERRIDE;
    QString getText(qreal pts) const Q_DECL_OVERRIDE;
    bool canRender() const Q_DECL_OVERRIDE;
    QImage getImage(qreal pts, QRect* boundingRect = 0) Q_DECL_OVERRIDE;
    void reset() Q_DECL_OVERRIDE;
private:
    bool parsePictureBasedSubtitle(AVSubtitleRect **rects, QImage &image, QPoint &pos);
    bool processSubtitle();
    AVCodecContext *codec_ctx;
    AVDemuxer m_reader;
    QList<SubtitleFrame> m_frames;
    AVSubtitleType m_subType;
};

static const SubtitleProcessorId SubtitleProcessorId_FFmpeg = QStringLiteral("qtav.subtitle.processor.ffmpeg");
namespace {
static const char kName[] = "FFmpeg";
}
FACTORY_REGISTER(SubtitleProcessor, FFmpeg, kName)

SubtitleProcessorFFmpeg::SubtitleProcessorFFmpeg()
    : codec_ctx(0),
      m_subType(SUBTITLE_NONE)
{
}

SubtitleProcessorFFmpeg::~SubtitleProcessorFFmpeg()
{
    avcodec_free_context(&codec_ctx);
}

SubtitleProcessorId SubtitleProcessorFFmpeg::id() const
{
    return SubtitleProcessorId_FFmpeg;
}

QString SubtitleProcessorFFmpeg::name() const
{
    return QLatin1String(kName);//SubtitleProcessorFactory::name(id());
}

QStringList ffmpeg_supported_sub_extensions_by_codec()
{
    QStringList exts;
    const AVCodec* c = NULL;
#if AVCODEC_STATIC_REGISTER
    void* it = NULL;
    while ((c = av_codec_iterate(&it))) {
#else
    avcodec_register_all();
    while ((c = av_codec_next(c))) {
#endif
        if (c->type != AVMEDIA_TYPE_SUBTITLE)
            continue;
        qDebug("sub codec: %s", c->name);
#if AVFORMAT_STATIC_REGISTER
        const AVInputFormat *i = NULL;
        void* it2 = NULL;
        while ((i = av_demuxer_iterate(&it2))) {
#else
        av_register_all(); // MUST register all input/output formats
        AVInputFormat *i = NULL;
        while ((i = av_iformat_next(i))) {
#endif
            if (!strcmp(i->name, c->name)) {
                qDebug("found iformat");
                if (i->extensions) {
                    exts.append(QString::fromLatin1(i->extensions).split(QLatin1Char(',')));
                } else {
                    qDebug("has no exts");
                    exts.append(QString::fromLatin1(i->name));
                }
                break;
            }
        }
        if (!i) {
            //qDebug("codec name '%s' is not found in AVInputFormat, just append codec name", c->name);
            //exts.append(c->name);
        }
    }
    return exts;
}

QStringList ffmpeg_supported_sub_extensions()
{
    QStringList exts;
#if AVFORMAT_STATIC_REGISTER
    const AVInputFormat *i = NULL;
    void* it = NULL;
    while ((i = av_demuxer_iterate(&it))) {
#else
    av_register_all(); // MUST register all input/output formats
    AVInputFormat *i = NULL;
    while ((i = av_iformat_next(i))) {
#endif
        // strstr parameters can not be null
        if (i->long_name && strstr(i->long_name, "subtitle")) {
            if (i->extensions) {
                exts.append(QString::fromLatin1(i->extensions).split(QLatin1Char(',')));
            } else {
                exts.append(QString::fromLatin1(i->name));
            }
        }
    }
    // AVCodecDescriptor.name and AVCodec.name may be different. avcodec_get_name() use AVCodecDescriptor if possible
    QStringList codecs;
    const AVCodec* c = NULL;
#if AVCODEC_STATIC_REGISTER
    it = NULL;
    while ((c = av_codec_iterate(&it))) {
#else
    avcodec_register_all();
    while ((c = av_codec_next(c))) {
#endif
        if (c->type == AVMEDIA_TYPE_SUBTITLE)
            codecs.append(QString::fromLatin1(c->name));
    }
    const AVCodecDescriptor *desc = NULL;
    while ((desc = avcodec_descriptor_next(desc))) {
        if (desc->type == AVMEDIA_TYPE_SUBTITLE)
            codecs.append(QString::fromLatin1(desc->name));
    }
    exts << codecs;
    exts.removeDuplicates();
    return exts;
}

QStringList SubtitleProcessorFFmpeg::supportedTypes() const
{
    // TODO: mp4. check avformat classes
#if 0
    typedef struct {
        const char* ext;
        const char* name;
    } sub_ext_t;
    static const sub_ext_t sub_ext[] = {
        { "ass", "ass" },
        { "ssa", "ass" },
        { "sub", "subviewer" },
        { ""
    }
    // from ffmpeg/tests/fate/subtitles.mak
    static const QStringList sSuffixes = QStringList() << "ass" << "ssa" << "sub" << "srt" << "txt" << "vtt" << "smi" << "pjs" << "jss" << "aqt";
#endif
    static const QStringList sSuffixes = ffmpeg_supported_sub_extensions();
    return sSuffixes;
}

bool SubtitleProcessorFFmpeg::process(QIODevice *dev)
{
    if (!dev->isOpen()) {
        if (!dev->open(QIODevice::ReadOnly)) {
            qWarning() << "open qiodevice error: " << dev->errorString();
            return false;
        }
    }
    m_reader.setMedia(dev);
    if (!m_reader.load())
        goto error;
    if (m_reader.subtitleStreams().isEmpty())
        goto error;
    qDebug("subtitle format: %s", m_reader.formatContext()->iformat->name);
    if (!processSubtitle())
        goto error;
    m_reader.unload();
    return true;
error:
    m_reader.unload();
    return false;
}

bool SubtitleProcessorFFmpeg::process(const QString &path)
{
    m_reader.setMedia(path);
    if (!m_reader.load())
        goto error;
    if (m_reader.subtitleStreams().isEmpty())
        goto error;
    qDebug("subtitle format: %s", m_reader.formatContext()->iformat->name);
    if (!processSubtitle())
        goto error;
    m_reader.unload();
    return true;
error:
    m_reader.unload();
    return false;
}

QList<SubtitleFrame> SubtitleProcessorFFmpeg::frames() const
{
    return m_frames;
}

QString SubtitleProcessorFFmpeg::getText(qreal pts) const
{
    QString text;
    for (int i = 0; i < m_frames.size(); ++i) {
        if (m_frames[i].begin <= pts && m_frames[i].end >= pts) {
            text += m_frames[i].text + QStringLiteral("\n");
            continue;
        }
        if (!text.isEmpty())
            break;
    }
    return text.trimmed();
}

bool SubtitleProcessorFFmpeg::processHeader(const QByteArray &codec, const QByteArray &data)
{
    Q_UNUSED(data);
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    AVCodec *c = avcodec_find_decoder_by_name(codec.constData());
    if (!c) {
        qDebug("subtitle avcodec_descriptor_get_by_name %s", codec.constData());
        const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(codec.constData());
        if (!desc) {
            qWarning("No codec descriptor found for %s", codec.constData());
            return false;
        }
        c = avcodec_find_decoder(desc->id);
    }
    if (!c) {
        qWarning("No subtitle decoder found for codec: %s, try fron descriptor", codec.constData());
        return false;
    }
    codec_ctx = avcodec_alloc_context3(c);
    if (!codec_ctx)
        return false;
    // no way to get time base. the pts unit used in processLine() is 's', ffmpeg use ms, so set 1/1000 here
    codec_ctx->time_base.num = 1;
    codec_ctx->time_base.den = 1000;
    if (!data.isEmpty()) {
        av_free(codec_ctx->extradata);
        codec_ctx->extradata = (uint8_t*)av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!codec_ctx->extradata)
            return false;
        codec_ctx->extradata_size = data.size();
        memcpy(codec_ctx->extradata, data.constData(), data.size());
    }
    AVDictionary *opts = NULL;
    Internal::setOptionsToDict(QVariant(optionsForSubtitleCodec()), &opts);
    if (avcodec_open2(codec_ctx, c, &opts) < 0) {
        av_dict_free(&opts);
        avcodec_free_context(&codec_ctx);
        return false;
    }
    av_dict_free(&opts);
    return true;//codec != QByteArrayLiteral("ass") && codec != QByteArrayLiteral("ssa");
}

SubtitleFrame SubtitleProcessorFFmpeg::processLine(const QByteArray &data, qreal pts, qreal duration)
{
    //qDebug() << "line: " << data;
    if (!codec_ctx) {
        return SubtitleFrame();
    }
    // AV_CODEC_ID_xxx and srt, subrip are available for ffmpeg >= 1.0. AV_CODEC_ID_xxx
    // TODO: what about other formats?
    // libav-9: packet data from demuxer contains time and but duration is 0, must decode
    // Always decode the data because it may contain styles
    if (false && duration > 0 && (!codec_ctx
#if QTAV_USE_FFMPEG(LIBAVCODEC)
            || codec_ctx->codec_id == AV_CODEC_ID_SUBRIP
#endif
            || codec_ctx->codec_id == AV_CODEC_ID_SRT
            )) {
        SubtitleFrame f;
        f.begin = pts;
        f.end = pts + duration;
        if (data.startsWith("Dialogue:")) // e.g. decoding embedded subtitles
            f.text = PlainText::fromAss(data.constData());
        else
            f.text = QString::fromUtf8(data.constData(), data.size()); //utf-8 is required
        return f;
    }
    AVPacket packet;
    av_init_packet(&packet);
    packet.size = data.size();
    packet.data = (uint8_t*)data.constData();
    /*
     * ffmpeg <2.5: AVPakcet.data has original text including time info, decoder use that info to get correct time
     * "Dialogue: 0,0:00:20.21,0:00:22.96,*Default,NTP,0000,0000,0000,blablabla
     * ffmpeg >=2.5: AVPakcet.data changed, we have to set correct pts & duration for packet to be decoded
     * 16,0,*Default,NTP,0000,0000,0000,,blablabla
     */
    // no codec_ctx for internal sub
    const double unit = 1.0/av_q2d(codec_ctx->time_base); //time_base is deprecated, use framerate since 17085a0, check FF_API_AVCTX_TIMEBASE
    packet.pts = pts * unit;
    packet.duration = duration * unit;
    AVSubtitle sub;
    memset(&sub, 0, sizeof(sub));
    int got_subtitle = 0;
    int ret = avcodec_decode_subtitle2(codec_ctx, &sub, &got_subtitle, &packet);
    if (ret < 0 || !got_subtitle) {
        av_packet_unref(&packet);
        avsubtitle_free(&sub);
        return SubtitleFrame();
    }
    SubtitleFrame frame;
    // start_display_time and duration are in ms
    frame.begin = pts + qreal(sub.start_display_time)/1000.0;
    frame.end = pts + qreal(sub.end_display_time)/1000.0;
    //qDebug() << QTime(0, 0, 0).addMSecs(frame.begin*1000.0) << "-" << QTime(0, 0, 0).addMSecs(frame.end*1000.0) << " fmt: " << sub.format << " pts: " << m_reader.packet().pts //sub.pts
      //       << " rects: " << sub.num_rects << " end: " << sub.end_display_time;
    for (unsigned i = 0; i < sub.num_rects; i++) {
        m_subType = sub.rects[i]->type;
        switch (sub.rects[i]->type) {
        case SUBTITLE_ASS:
            //qDebug("ass frame: %s", sub.rects[i]->ass);
            frame.text.append(PlainText::fromAss(sub.rects[i]->ass)).append(ushort('\n'));
            break;
        case SUBTITLE_TEXT:
            //qDebug("txt frame: %s", sub.rects[i]->text);
            frame.text.append(QString::fromUtf8(sub.rects[i]->text)).append(ushort('\n'));
            break;
        case SUBTITLE_BITMAP:
            //sub.rects[i]->w > 0 && sub.rects[i]->h > 0
            //qDebug("bmp sub");
            //frame = SubtitleFrame(); // not support bmp subtitle now
            break;
        default:
            break;
        }
    }
    if (sub.num_rects > 0 && sub.rects[0]->type == SUBTITLE_BITMAP) {
        parsePictureBasedSubtitle(sub.rects, frame.img, frame.pos);
    }
    av_packet_unref(&packet);
    avsubtitle_free(&sub);
    return frame;
}

bool SubtitleProcessorFFmpeg::canRender() const
{
    return m_subType == SUBTITLE_BITMAP;
}

QImage SubtitleProcessorFFmpeg::getImage(qreal pts, QRect* boundingRect)
{
    return QImage();
}

void SubtitleProcessorFFmpeg::reset()
{
    m_frames.clear();
}

bool SubtitleProcessorFFmpeg::parsePictureBasedSubtitle(AVSubtitleRect **rects, QImage &image, QPoint &pos)
{
    AVSubtitleRect *sub_rect = rects[0];
    struct SwsContext *sub_convert_ctx = NULL;
    AVFrame *picture = NULL;

    image.fill(Qt::transparent);
    picture = av_frame_alloc();
    if (!picture) {
        return false;
    }
    if (avpicture_alloc((AVPicture *)picture, AV_PIX_FMT_RGB32, sub_rect->w, sub_rect->h) < 0) {
        av_frame_free(&picture);
        return false;
    }
    sub_convert_ctx = sws_getContext(
        sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
        sub_rect->w, sub_rect->h, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, NULL, NULL, NULL);
    if (!sub_convert_ctx) {
        avpicture_free((AVPicture *)picture);
        av_frame_free(&picture);
        return false;
    }
    sws_scale(sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
        0, sub_rect->h, picture->data, picture->linesize);
    image = QImage((const uchar*)picture->data[0], sub_rect->w, sub_rect->h, picture->linesize[0], QImage::Format_ARGB32).copy();
    //VideoFrame frame(sub_rect->w, sub_rect->h, VideoFormat(VideoFormat::Format_RGBA32));
    //frame.setBits(picture->data);
    //frame.setBytesPerLine(picture->linesize);
    //image = frame.toImage(QImage::Format_ARGB32);
    //FIXME giga:remove the line at the left of the image, about 2px width
    //image = image.copy(2, 0, image.width() - 2, image.height());
    pos = QPoint(sub_rect->x, sub_rect->y);
    sws_freeContext(sub_convert_ctx);
    avpicture_free((AVPicture *)picture);
    av_frame_free(&picture);
    return true;
}

bool SubtitleProcessorFFmpeg::processSubtitle()
{
    m_frames.clear();
    int ss = m_reader.subtitleStream();
    if (ss < 0) {
        qWarning("no subtitle stream found");
        return false;
    }
    codec_ctx = m_reader.subtitleCodecContext();
    AVCodec *dec = avcodec_find_decoder(codec_ctx->codec_id);
    const AVCodecDescriptor *dec_desc = avcodec_descriptor_get(codec_ctx->codec_id);
    if (!dec) {
        if (dec_desc)
            qWarning("Failed to find subtitle codec %s", dec_desc->name);
        else
            qWarning("Failed to find subtitle codec %d", codec_ctx->codec_id);
        return false;
    }
    qDebug("found subtitle decoder '%s'", dec_desc->name);
    // AV_CODEC_PROP_TEXT_SUB: ffmpeg >= 2.0
#ifdef AV_CODEC_PROP_TEXT_SUB
    if (dec_desc && !(dec_desc->props & AV_CODEC_PROP_TEXT_SUB)) {
        qWarning("Only text based subtitles are currently supported");
        return false;
    }
#endif
    AVDictionary *codec_opts = NULL;
    int ret = avcodec_open2(codec_ctx, dec, &codec_opts);
    if (ret < 0) {
        qWarning("open subtitle codec error: %s", av_err2str(ret));
        av_dict_free(&codec_opts);
        return false;
    }
    while (!m_reader.atEnd()) {
        if (!m_reader.readFrame()) { // eof or other errors
            continue;
        }
        if (m_reader.stream() != ss)
            continue;
        const Packet pkt = m_reader.packet();
        if (!pkt.isValid())
            continue;
        SubtitleFrame frame = processLine(pkt.data, pkt.pts, pkt.duration);
        if (frame.isValid())
            m_frames.append(frame);
    }
    avcodec_close(codec_ctx);
    codec_ctx = 0;
    return true;
}

} //namespace QtAV
