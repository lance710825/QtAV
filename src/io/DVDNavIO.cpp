#include "MediaIO.h"
#include "QtAV/private/MediaIO_p.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include "QtAV/private/prepost.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>
#include <dvdnav/dvd_types.h>
#include <dvdnav/dvdnav.h>
#ifdef __cplusplus
}
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef WIN32
#pragma comment(lib,"wsock32.lib")
#include <windows.h>
#endif

#include <QtCore/QIODevice>
#include <QDebug>

#define FFMin(a, b) (a < b) ? a : b
#define FFMax(a, b) (a > b) ? a : b
#define STREAM_BUFFER_MIN 2048
#define STREAM_BUFFER_SIZE (STREAM_BUFFER_MIN) // must be at least 2*STREAM_BUFFER_MIN
#define STREAM_MAX_SECTOR_SIZE (8*1024)
#define STREAM_REDIRECTED -2
#define STREAM_UNSUPPORTED -1
#define STREAM_ERROR 0
#define STREAM_OK    1
#define MAX_STREAM_PROTOCOLS 10

#define HAVE_WINSOCK2_H 1

#define STREAM_CTRL_RESET 0
#define STREAM_CTRL_GET_TIME_LENGTH 1
#define STREAM_CTRL_SEEK_TO_CHAPTER 2
#define STREAM_CTRL_GET_CURRENT_CHAPTER 3
#define STREAM_CTRL_GET_NUM_CHAPTERS 4
#define STREAM_CTRL_GET_CURRENT_TIME 5
#define STREAM_CTRL_SEEK_TO_TIME 6
#define STREAM_CTRL_GET_SIZE 7
#define STREAM_CTRL_GET_ASPECT_RATIO 8
#define STREAM_CTRL_GET_NUM_ANGLES 9
#define STREAM_CTRL_GET_ANGLE 10
#define STREAM_CTRL_SET_ANGLE 11
#define STREAM_CTRL_GET_NUM_TITLES 12
#define STREAM_CTRL_GET_LANG 13
#define STREAM_CTRL_GET_CURRENT_TITLE 14
#define STREAM_CTRL_GET_CURRENT_CHANNEL 15

/*Demuxer*/
#define DEMUXER_TYPE_UNKNOWN 0
#define DEMUXER_TYPE_MPEG_ES 1
#define DEMUXER_TYPE_MPEG_PS 2

/*Stream Type*/
#define STREAMTYPE_DVD  3      // libdvdread
#define STREAMTYPE_DVDNAV 9    // we cannot safely "seek" in this...

#define STREAM_READ  0
#define STREAM_WRITE 1

namespace QtAV {

const char * const dvd_audio_stream_types[8] = { "ac3","unknown","mpeg1","mpeg2ext","lpcm","unknown","dts" };
const char * const dvd_audio_stream_channels[6] = { "mono", "stereo", "unknown", "unknown", "5.1/6.1", "5.1" };

class DVDNavIOPrivate;
typedef struct stream {
    int fd;   // file descriptor, see man open(2)
    int type; // see STREAMTYPE_*
    int flags;
    int sector_size; // sector size (seek will be aligned on this size if non 0)
    int read_chunk; // maximum amount of data to read at once to limit latency (0 for default)
    unsigned int buf_pos,buf_len;
    int64_t pos,start_pos,end_pos;
    int eof;
    int mode; //STREAM_READ or STREAM_WRITE
    char* url;  // strdup() of filename/url
} stream_t;

typedef enum {
    NAV_FLAG_EOF                  = 1 << 0,  /* end of stream has been reached */
    NAV_FLAG_WAIT                 = 1 << 1,  /* wait event */
    NAV_FLAG_WAIT_SKIP            = 1 << 2,  /* wait skip disable */
    NAV_FLAG_CELL_CHANGE          = 1 << 3,  /* cell change event */
    NAV_FLAG_WAIT_READ_AUTO       = 1 << 4,  /* wait read auto mode */
    NAV_FLAG_WAIT_READ            = 1 << 5,  /* suspend read from stream */
    NAV_FLAG_VTS_DOMAIN           = 1 << 6,  /* vts domain */
    NAV_FLAG_SPU_SET              = 1 << 7,  /* spu_clut is valid */
    NAV_FLAG_STREAM_CHANGE        = 1 << 8,  /* title, chapter, audio or SPU */
    NAV_FLAG_AUDIO_CHANGE         = 1 << 9,  /* audio stream change event */
    NAV_FLAG_SPU_CHANGE           = 1 << 10, /* spu stream change event */
} dvdnav_state_t;

typedef struct {
    dvdnav_t *       dvdnav;              /* handle to libdvdnav stuff */
    unsigned int     duration;            /* in milliseconds */
    int              mousex, mousey;
    int              title;
    unsigned int     spu_clut[16];
    dvdnav_highlight_event_t hlev;
    int              still_length;        /* still frame duration */
    unsigned int     state;
} dvdnav_priv_t;

typedef struct {
    uint16_t sx, sy;
    uint16_t ex, ey;
    uint32_t palette;
} nav_highlight_t;

enum stream_ctrl_type {
    stream_ctrl_audio,
    stream_ctrl_sub
};

enum file_system {
    AUTO = 0,
    UDF,
    ISO9660
};

static bool compareParts(int part1, int part2)
{
    return part1 < part2;
}

struct stream_lang_req {
    enum stream_ctrl_type type;
    int id;
    char buf[40];
};

class DVDNavIO : public MediaIO
{
    Q_OBJECT
    DPTR_DECLARE_PRIVATE(DVDNavIO)
public:
    DVDNavIO();
    virtual QString name() const Q_DECL_OVERRIDE;

    const QStringList& protocols() const Q_DECL_OVERRIDE
    {
        static QStringList p = QStringList()
                << QStringLiteral("dvdnav");
        return p;
    }

    virtual bool isSeekable() const Q_DECL_OVERRIDE;
    virtual bool isWritable() const Q_DECL_OVERRIDE;
    virtual qint64 read(char *data, qint64 maxSize) Q_DECL_OVERRIDE;
    virtual qint64 write(const char *data, qint64 maxSize) Q_DECL_OVERRIDE;
    virtual bool seek(qint64 offset, int from) Q_DECL_OVERRIDE;
    virtual qint64 position() const Q_DECL_OVERRIDE;
    /*!
    * \brief size
    * \return <=0 if not support
    */
    virtual qint64 size() const Q_DECL_OVERRIDE;
    virtual qint64 clock() Q_DECL_OVERRIDE;
    virtual qint64 startTimeUs() Q_DECL_OVERRIDE;
    virtual qint64 duration() Q_DECL_OVERRIDE;
    virtual void setDuration(qint64 duration) Q_DECL_OVERRIDE;
    virtual void setOptionsForIOCodec(const QVariantHash & dict) Q_DECL_OVERRIDE;

protected:
    DVDNavIO(DVDNavIOPrivate &d);

    void onUrlChanged() Q_DECL_OVERRIDE;

};
typedef DVDNavIO MediaIODVDNav;
static const MediaIOId MediaIOId_DVDNav = mkid::id32base36_6<'d', 'v', 'd', 'n', 'a', 'v'>::value;
static const char kDVDNavDevName[] = "dvdnav";
FACTORY_REGISTER(MediaIO, DVDNav, kDVDNavDevName)

class DVDNavIOPrivate : public MediaIOPrivate
{
public:
    DVDNavIOPrivate(): MediaIOPrivate(),
        dvd_stream(NULL),
        priv(NULL),
        dvd_angle(0),
        dvd_last_chapter(1),
        current_title(0),
        current_chapter(0),
        current_chapter_index(0),
        started(false),
        current_start_time(0),
        current_time(0),
        current_duration(0),
        custom_duration(0),
        current_cell(0),
        file_sys(UDF),
        nb_of_chapter(0),
        cur_chapter_index(0),
        skip_nav_pack(false),
        stopped(false)
    {

    }
    ~DVDNavIOPrivate()
    {
        stream_dvdnav_close();
    }
    int  stream_dvdnav_open(const char *filename);
    int  stream_dvdnav_read(stream_t *s, char *but, int len);
    bool stream_dvdnav_seek(stream_t *s, int64_t newpos);
    int  stream_dvdnav_control(stream_t *stream, int cmd, void* arg);
    void stream_dvdnav_close();
    void stream_dvdnav_get_time();
    int64_t update_current_time();

public:
    stream *dvd_stream;
    dvdnav_priv_t *priv;
    int current_title;
    int current_chapter;
    int current_chapter_index;
    QList<int> parts;
    int dvd_angle;
    int dvd_last_chapter;
    bool started;
    qint64 current_start_time;
    qint64 current_time;
    qint64 current_duration;
    qint64 custom_duration;
    int current_cell;
    file_system file_sys;
    int error_count;
    int clock_flag;

    int nb_of_chapter;
    int cur_chapter_index;
    bool skip_nav_pack;
    bool stopped;

private:
    stream_t* new_stream(int fd,int type);
    int  get_block(uint8_t *buf, int *event);
    int  dvdnav_first_play();
    void dvdnav_get_highlight(dvdnav_priv_t *priv, int display_mode);
    int  dvdnav_get_duration(int length);
    int  dvdnav_stream_read(dvdnav_priv_t * priv, unsigned char *buf, int *len);
    void update_title_len(stream_t *stream);
    int  dvdtimetomsec(dvd_time_t *dt);
    void show_audio_subs_languages(dvdnav_t *nav);
    int  dvdnav_lang_from_aid(int aid);
    int  dvdnav_lang_from_sid(int sid);
};

DVDNavIO::DVDNavIO() : MediaIO(*new DVDNavIOPrivate()) {}
DVDNavIO::DVDNavIO(DVDNavIOPrivate &d) : MediaIO(d) {}
QString DVDNavIO::name() const { return QLatin1String(kDVDNavDevName); }

bool DVDNavIO::isSeekable() const
{
    return true;
}

bool DVDNavIO::isWritable() const
{
    return false;
}

qint64 DVDNavIO::read(char *data, qint64 maxSize)
{
    DPTR_D(DVDNavIO);

    Q_UNUSED(maxSize)
    if (!d.dvd_stream)
        return AVERROR_EOF;
    int len = 0;

    len = d.stream_dvdnav_read(d.dvd_stream, data, STREAM_BUFFER_SIZE);
    if (len <= 0) {
        return AVERROR_EOF;
    }
    return len;
}

qint64 DVDNavIO::write(const char *data, qint64 maxSize)
{
    Q_UNUSED(data)
    Q_UNUSED(maxSize)
    return 0;;
}

bool DVDNavIO::seek(qint64 offset, int from)
{
    DPTR_D(DVDNavIO);

    Q_UNUSED(from)
    if (!d.dvd_stream)
        return false;

    if (from == -200) {
        uint32_t newpos, len;
        if (dvdnav_time_search(d.priv->dvdnav, offset * 90) != DVDNAV_STATUS_OK)
            return false;
        char buf[STREAM_BUFFER_SIZE];
        dvdnav_status_t status = DVDNAV_STATUS_ERR;
        while (status != DVDNAV_STATUS_OK) {
            d.stream_dvdnav_read(d.dvd_stream, buf, STREAM_BUFFER_SIZE);
            status = dvdnav_get_position(d.priv->dvdnav, &newpos, &len);
        }
        d.dvd_stream->pos = newpos;
        return true;
    }

    bool flag = d.stream_dvdnav_seek(d.dvd_stream, offset);
    return flag;
}

qint64 DVDNavIO::position() const
{
    DPTR_D(const DVDNavIO);
    if (!d.dvd_stream)
        return 0;
    return d.dvd_stream->pos;
}

qint64 DVDNavIO::size() const
{
    DPTR_D(const DVDNavIO);
    if (!d.dvd_stream)
        return 0;
    return d.dvd_stream->end_pos - d.dvd_stream->start_pos; // sequential device returns bytesAvailable()
}

qint64 DVDNavIO::clock()
{
    DPTR_D(DVDNavIO);

    int64_t time = 0;

    if (!d.dvd_stream || !d.priv)
        return 0;
    if (d.priv && (d.priv->state & NAV_FLAG_EOF)) {
        return d.current_duration / 1000;
    }
    time = d.update_current_time();
    if (d.clock_flag) {
        --d.clock_flag;
        return 0;
    }
    return time;/*ms*/
}

qint64 DVDNavIO::startTimeUs()
{
    DPTR_D(DVDNavIO);

    return d.current_start_time;
}

qint64 DVDNavIO::duration()
{
    DPTR_D(DVDNavIO);

    return d.current_duration;
}

void DVDNavIO::setDuration(qint64 duration)
{
    DPTR_D(DVDNavIO);

    d.custom_duration = duration;
}

void DVDNavIO::setOptionsForIOCodec(const QVariantHash &dict)
{
    DPTR_D(DVDNavIO);

    QVariant opt(dict);
    if (dict.contains("filesys")) {
        int sys = dict.value("filesys").toInt();
        if (sys >= AUTO && sys <= ISO9660) {
            d.file_sys = (file_system)sys;
        }
    }
    if (dict.contains("title")) {
        d.current_title = dict.value("title").toInt();
    }
    if (dict.contains("chapter")) {
        QList<QVariant> lstParts = dict.value("chapter").toList();
        d.parts.clear();
        for (int i = 0; i < lstParts.count(); ++i) {
            d.parts.append(lstParts.at(i).toInt());
        }
        qSort(d.parts.begin(), d.parts.end(), compareParts);
    }
}

void DVDNavIO::onUrlChanged()
{
    DPTR_D(DVDNavIO);
    QString path(url());
    QString filename, titleInfo;

    if (path.startsWith("dvdnav:")) {
        path = path.mid(9); //remove "dvdnav://"
        d.stream_dvdnav_open(path.toUtf8().constData());
    }
}

stream_t* DVDNavIOPrivate::new_stream(int fd, int type)
{
    stream_t *s = (stream_t *)calloc(1, sizeof(stream_t));
    if(s == NULL) return NULL;

    s->fd=fd;
    s->type=type;
    s->buf_pos=s->buf_len=0;
    s->start_pos=s->end_pos=0;
    if(s->eof){
        s->buf_pos=s->buf_len=0;
        s->eof=0;
    }
    stream_dvdnav_control(s, STREAM_CTRL_RESET, NULL);
    //stream_seek(s,0);
    return s;
}

void DVDNavIOPrivate::dvdnav_get_highlight(dvdnav_priv_t *priv, int display_mode)
{
    pci_t *pnavpci = NULL;
    dvdnav_highlight_event_t *hlev = &(priv->hlev);
    uint32_t btnum;

    if (!priv || !priv->dvdnav)
        return;

    pnavpci = dvdnav_get_current_nav_pci (priv->dvdnav);
    if (!pnavpci)
        return;

    dvdnav_get_current_highlight (priv->dvdnav, (int32_t *)&(hlev->buttonN) );
    hlev->display = display_mode; /* show */

    if (hlev->buttonN > 0 && pnavpci->hli.hl_gi.btn_ns > 0 && hlev->display) {
        for (btnum = 0; btnum < pnavpci->hli.hl_gi.btn_ns; btnum++) {
            btni_t *btni = &(pnavpci->hli.btnit[btnum]);

            if (hlev->buttonN == btnum + 1) {
                hlev->sx = FFMin (btni->x_start, btni->x_end);
                hlev->ex = FFMax (btni->x_start, btni->x_end);
                hlev->sy = FFMin (btni->y_start, btni->y_end);
                hlev->ey = FFMax (btni->y_start, btni->y_end);

                hlev->palette = (btni->btn_coln == 0) ? 0 :
                                                        pnavpci->hli.btn_colit.btn_coli[btni->btn_coln - 1][0];
                break;
            }
        }
    } else { /* hide button or no button */
        hlev->sx = hlev->ex = 0;
        hlev->sy = hlev->ey = 0;
        hlev->palette = hlev->buttonN = 0;
    }
}

int DVDNavIOPrivate::dvdnav_get_duration(int length)
{
    return (length == 255) ? 0 : length * 1000;
}

int DVDNavIOPrivate::dvdtimetomsec(dvd_time_t *dt)
{
    static int framerates[4] = {0, 2500, 0, 2997};
    int framerate = framerates[(dt->frame_u & 0xc0) >> 6];
    int msec = (((dt->hour & 0xf0) >> 3) * 5 + (dt->hour & 0x0f)) * 3600000;
    msec += (((dt->minute & 0xf0) >> 3) * 5 + (dt->minute & 0x0f)) * 60000;
    msec += (((dt->second & 0xf0) >> 3) * 5 + (dt->second & 0x0f)) * 1000;
    if(framerate > 0)
        msec += (((dt->frame_u & 0x30) >> 3) * 5 + (dt->frame_u & 0x0f)) * 100000 / framerate;
    return msec;
}

void DVDNavIOPrivate::show_audio_subs_languages(dvdnav_t *nav)
{
    uint8_t lg;
    uint16_t i, lang, format, id, channels;
    int base[7] = {128, 0, 0, 0, 160, 136, 0};
    for(i=0; i<8; i++)
    {
        char tmp[] = "unknown";
        lg = dvdnav_get_audio_logical_stream(nav, i);
        if(lg == 0xff) continue;
        channels = dvdnav_audio_stream_channels(nav, lg);
        if(channels == 0xFFFF)
            channels = 2; //unknown
        else
            channels--;
        lang = dvdnav_audio_stream_to_lang(nav, lg);
        if(lang != 0xFFFF)
        {
            tmp[0] = lang >> 8;
            tmp[1] = lang & 0xFF;
            tmp[2] = 0;
        }
        format = dvdnav_audio_stream_format(nav, lg);
        if(format == 0xFFFF || format > 6)
            format = 1; //unknown
        id = i + base[format];
        qDebug("audio stream: %d format: %s (%s) language: %s aid: %d.\n", i,
               dvd_audio_stream_types[format], dvd_audio_stream_channels[channels], tmp, id);
        if (lang != 0xFFFF && lang && tmp[0])
            qDebug("ID_AID_%d_LANG=%s\n", id, tmp);
    }

    for(i=0; i<32; i++)
    {
        char tmp[] = "unknown";
        lg = dvdnav_get_spu_logical_stream(nav, i);
        if(lg == 0xff) continue;
        lang = dvdnav_spu_stream_to_lang(nav, i);
        if(lang != 0xFFFF)
        {
            tmp[0] = lang >> 8;
            tmp[1] = lang & 0xFF;
            tmp[2] = 0;
        }
        qDebug("subtitle ( sid ): %d language: %s\n", lg, tmp);
        if (lang != 0xFFFF && lang && tmp[0])
            qDebug("ID_SID_%d_LANG=%s\n", lg, tmp);
    }
}

/**
 * \brief mp_dvdnav_lang_from_aid() returns the language corresponding to audio id 'aid'
 * \param stream: - stream pointer
 * \param sid: physical subtitle id
 * \return 0 on error, otherwise language id
 */
int DVDNavIOPrivate::dvdnav_lang_from_aid(int aid)
{
    uint8_t lg;
    uint16_t lang;

    if(aid < 0)
        return 0;
    lg = dvdnav_get_audio_logical_stream(priv->dvdnav, aid & 0x7);
    if(lg == 0xff) return 0;
    lang = dvdnav_audio_stream_to_lang(priv->dvdnav, lg);
    if(lang == 0xffff) return 0;
    return lang;
}
/**
 * \brief mp_dvdnav_lang_from_sid() returns the language corresponding to subtitle id 'sid'
 * \param stream: - stream pointer
 * \param sid: physical subtitle id
 * \return 0 on error, otherwise language id
 */
int DVDNavIOPrivate::dvdnav_lang_from_sid(int sid)
{
    uint8_t k;
    uint16_t lang;

    if(sid < 0) return 0;
    for (k=0; k<32; k++)
        if (dvdnav_get_spu_logical_stream(priv->dvdnav, k) == sid)
            break;
    if (k == 32)
        return 0;
    lang = dvdnav_spu_stream_to_lang(priv->dvdnav, k);
    if(lang == 0xffff) return 0;
    return lang;
}

int DVDNavIOPrivate::stream_dvdnav_control(stream_t *stream, int cmd, void* arg)
{
    int tit, part;

    switch(cmd)
    {
    case STREAM_CTRL_SEEK_TO_CHAPTER:
    {
        int chap = *(unsigned int *)arg+1;

        if(chap < 1 || dvdnav_current_title_info(priv->dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        if(dvdnav_part_play(priv->dvdnav, tit, chap) != DVDNAV_STATUS_OK)
            break;
        return 1;
    }
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    {
        if(dvdnav_current_title_info(priv->dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        if(dvdnav_get_number_of_parts(priv->dvdnav, tit, &part) != DVDNAV_STATUS_OK)
            break;
        if(!part)
            break;
        *(unsigned int *)arg = part;
        return 1;
    }
    case STREAM_CTRL_GET_CURRENT_CHAPTER:
    {
        if(dvdnav_current_title_info(priv->dvdnav, &tit, &part) != DVDNAV_STATUS_OK)
            break;
        *(unsigned int *)arg = part - 1;
        return 1;
    }
    case STREAM_CTRL_GET_TIME_LENGTH:
    {
        if(priv->duration || priv->still_length)
        {
            *(double *)arg = (double)priv->duration / 1000.0;
            return 1;
        }
        break;
    }
    case STREAM_CTRL_GET_ASPECT_RATIO:
    {
        uint8_t ar = dvdnav_get_video_aspect(priv->dvdnav);
        *(double *)arg = !ar ? 4.0/3.0 : 16.0/9.0;
        return 1;
    }
    case STREAM_CTRL_GET_CURRENT_TIME:
    {
        double tm;
        tm = dvdnav_get_current_time(priv->dvdnav)/90000.0f;
        if(tm != -1)
        {
            *(double *)arg = tm;
            return 1;
        }
        break;
    }
    case STREAM_CTRL_SEEK_TO_TIME:
    {
        uint64_t tm = *(double *)arg * 90000;
        if(dvdnav_time_search(priv->dvdnav, tm) == DVDNAV_STATUS_OK)
            return 1;
        break;
    }
    case STREAM_CTRL_GET_NUM_ANGLES:
    {
        int32_t curr, angles;
        if(dvdnav_get_angle_info(priv->dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = angles;
        return 1;
    }
    case STREAM_CTRL_GET_ANGLE:
    {
        int32_t curr, angles;
        if(dvdnav_get_angle_info(priv->dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        *(int *)arg = curr;
        return 1;
    }
    case STREAM_CTRL_SET_ANGLE:
    {
        int32_t curr, angles;
        int new_angle = *(int *)arg;
        if(dvdnav_get_angle_info(priv->dvdnav, &curr, &angles) != DVDNAV_STATUS_OK)
            break;
        if(new_angle>angles || new_angle<1)
            break;
        if(dvdnav_angle_change(priv->dvdnav, new_angle) != DVDNAV_STATUS_OK)
            return 1;
    }
    case STREAM_CTRL_GET_LANG:
    {
        struct stream_lang_req *req = (stream_lang_req *)arg;
        int lang = 0;
        switch(req->type) {
        case stream_ctrl_audio:
            lang = dvdnav_lang_from_aid(req->id);
            break;
        case stream_ctrl_sub:
            lang = dvdnav_lang_from_sid(req->id);
            break;
        }
        if (!lang)
            break;
        req->buf[0] = lang >> 8;
        req->buf[1] = lang;
        req->buf[2] = 0;
        return STREAM_OK;
    }
    }

    return STREAM_UNSUPPORTED;
}
int DVDNavIOPrivate::dvdnav_stream_read(dvdnav_priv_t * priv, unsigned char *buf, int *len)
{
    int event = DVDNAV_NOP;

    if (dvdnav_get_next_block(priv->dvdnav, buf, &event, len) != DVDNAV_STATUS_OK) {
        ++error_count;
        if (dvdnav_sector_search(priv->dvdnav, 1, SEEK_CUR) == DVDNAV_STATUS_ERR) {
            qDebug("sector search error : %s\n", dvdnav_err_to_string(priv->dvdnav));
            *len = 0;
        }
    }
    if (event != DVDNAV_BLOCK_OK && event != DVDNAV_NAV_PACKET) {
        *len = 0;
    }
    return event;
}

#if 1
int DVDNavIOPrivate::stream_dvdnav_read(stream_t *s, char *buf, int len)
{
    int event;

    if (current_title > 0 && (priv->state & NAV_FLAG_EOF))
        return 0;
    if (priv->state & NAV_FLAG_WAIT_READ) /* read is suspended */
        return -1;
    len = 0;
    if (!s->end_pos)
        update_title_len(s);
    error_count = 0;
    while (!len) /* grab all event until DVDNAV_BLOCK_OK (len=2048), DVDNAV_STOP or DVDNAV_STILL_FRAME */
    {
        if (error_count >= 100) {
            qDebug() << "Too many consecutive errors read!\n";
            return 0;
        }
        event = dvdnav_stream_read(priv, (unsigned char*)buf, &len);
        if (event == -1 || len == -1)
        {
            qDebug("DVDNAV stream read error!\n");
            return 0;
        }
        if (event != DVDNAV_BLOCK_OK) {
            dvdnav_get_highlight(priv, 1);
        }
        switch (event) {
        case DVDNAV_STILL_FRAME: {
            dvdnav_still_event_t *still_event = (dvdnav_still_event_t *)buf;
            priv->still_length = still_event->length;
            /* set still frame duration */
            priv->duration = dvdnav_get_duration(priv->still_length);
            if (priv->still_length <= 1) {
                pci_t *pnavpci = dvdnav_get_current_nav_pci(priv->dvdnav);
                priv->duration = dvdtimetomsec(&pnavpci->pci_gi.e_eltm);
            }
            return 0;
        }
        case DVDNAV_HIGHLIGHT: {
            dvdnav_get_highlight(priv, 1);
            break;
        }
        case DVDNAV_SPU_CLUT_CHANGE: {
            memcpy(priv->spu_clut, buf, 16 * sizeof(unsigned int));
            priv->state |= NAV_FLAG_SPU_SET;
            break;
        }
        case DVDNAV_STOP: {
            priv->state |= NAV_FLAG_EOF;
            return len;
        }
        case DVDNAV_NAV_PACKET:
            break;
        case DVDNAV_BLOCK_OK:
            return len;
        case DVDNAV_WAIT: {
            if ((priv->state & NAV_FLAG_WAIT_SKIP) &&
                (priv->state & NAV_FLAG_WAIT))
                dvdnav_wait_skip(priv->dvdnav);
            else {
                priv->state |= NAV_FLAG_WAIT_SKIP;
                priv->state |= NAV_FLAG_WAIT;
            }
            if (priv->state & NAV_FLAG_WAIT)
                return len;
            break;
        }
        case DVDNAV_VTS_CHANGE: {
            int tit = 0, part = 0;
            current_cell = 0;
            dvdnav_vts_change_event_t *vts_event = (dvdnav_vts_change_event_t *)buf;
            qDebug("DVDNAV, switched to title: %d\r\n", vts_event->new_vtsN);
            priv->state |= NAV_FLAG_CELL_CHANGE;
            priv->state |= NAV_FLAG_AUDIO_CHANGE;
            priv->state |= NAV_FLAG_SPU_CHANGE;
            priv->state |= NAV_FLAG_STREAM_CHANGE;
            priv->state &= ~NAV_FLAG_WAIT_SKIP;
            priv->state &= ~NAV_FLAG_WAIT;
            s->end_pos = 0;
            update_title_len(s);
            show_audio_subs_languages(priv->dvdnav);
            if (priv->state & NAV_FLAG_WAIT_READ_AUTO)
                priv->state |= NAV_FLAG_WAIT_READ;
            if (dvdnav_current_title_info(priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK) {
                qDebug("\r\nDVDNAV, NEW TITLE %d\r\n", tit);
                dvdnav_get_highlight(priv, 0);
                if (current_title > 0 && tit != current_title && started) {
                    priv->state |= NAV_FLAG_EOF;
                    return 0;
                }
            }
            break;
        }
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t *ev = (dvdnav_cell_change_event_t*)buf;
            uint32_t nextstill;

            priv->state &= ~NAV_FLAG_WAIT_SKIP;
            priv->state |= NAV_FLAG_STREAM_CHANGE;
            if (ev->pgc_length)
                priv->duration = ev->pgc_length / 90;

            if (dvdnav_is_domain_vts(priv->dvdnav)) {
                qDebug("DVDNAV_TITLE_IS_MOVIE\n");
                priv->state &= ~NAV_FLAG_VTS_DOMAIN;
            }
            else {
                qDebug("DVDNAV_TITLE_IS_MENU\n");
                priv->state |= NAV_FLAG_VTS_DOMAIN;
            }

            nextstill = dvdnav_get_next_still_flag(priv->dvdnav);
            if (nextstill) {
                priv->duration = dvdnav_get_duration(nextstill);
                priv->still_length = nextstill;
                if (priv->still_length <= 1) {
                    pci_t *pnavpci = dvdnav_get_current_nav_pci(priv->dvdnav);
                    priv->duration = dvdtimetomsec(&pnavpci->pci_gi.e_eltm);
                }
            }

            priv->state |= NAV_FLAG_CELL_CHANGE;
            priv->state |= NAV_FLAG_AUDIO_CHANGE;
            priv->state |= NAV_FLAG_SPU_CHANGE;
            priv->state &= ~NAV_FLAG_WAIT_SKIP;
            priv->state &= ~NAV_FLAG_WAIT;
            if (priv->state & NAV_FLAG_WAIT_READ_AUTO)
                priv->state |= NAV_FLAG_WAIT_READ;
//            if (current_title > 0 && s->end_pos > 0) {
//                int title, part;
//                if (dvdnav_current_title_info(priv->dvdnav, &title, &part) == DVDNAV_STATUS_OK) {
//                    if (parts.count() > 0 && current_chapter != part) {
//                        if (current_chapter_index >= parts.count()) {
//                            priv->state |= NAV_FLAG_EOF;
//                            return 0;
//                        }
//                        else {
//                            current_chapter = parts.at(current_chapter_index);
//                            ++current_chapter_index;
//                            if (dvdnav_part_search(priv->dvdnav, current_chapter) != DVDNAV_STATUS_OK) {
//                                qDebug("Couldn't select title %d, part %d, error '%s'\n", current_title, current_chapter, dvdnav_err_to_string(priv->dvdnav));
//                                priv->state |= NAV_FLAG_EOF;
//                                return 0;
//                            }
//                        }
//                    }
//                }
//            }
            //No matter for now
            //if (started && current_cell == ev->cellN) {
            //    priv->state |= NAV_FLAG_EOF;
            //    return 0;
            //}
            //current_cell = ev->cellN;
            dvdnav_get_highlight(priv, 1);
            break;
        }
        case DVDNAV_AUDIO_STREAM_CHANGE:
            priv->state |= NAV_FLAG_AUDIO_CHANGE;
            break;
        case DVDNAV_SPU_STREAM_CHANGE:
            priv->state |= NAV_FLAG_SPU_CHANGE;
            priv->state |= NAV_FLAG_STREAM_CHANGE;
            break;
        }
    }
    return len;
}
#else
int DVDNavIOPrivate::stream_dvdnav_read(stream_t *s, char *buf, int len)
{
    int event = 0;

    if (!s->end_pos) {
        update_title_len(s);
    }
    while (1) {
        int ret;

        if (stopped) {
            return AVERROR_EOF;
        }
        ret = get_block((uint8_t*)buf, &event);
        if (ret < 0) {
            return AVERROR_EOF;
        }
        switch (event) {
        case DVDNAV_BLOCK_OK: {
            return ret;
        }
        case DVDNAV_VTS_CHANGE: {
            int32_t title, cur_part;
            dvdnav_current_title_info(priv->dvdnav, &title, &cur_part);

            if (current_title && title != current_title) {
                // Transition to another title signals that we are done.
                qDebug("vts change, found next title %d to %d\n", current_title, title);
                stopped = true;
                return AVERROR_EOF;
            }
            current_title = title;
            break;
        }
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t * cell_event;
            cell_event = (dvdnav_cell_change_event_t*)buf;

            if (nb_of_chapter > 0) {
                if (nb_of_chapter <= cur_chapter_index) {
                    stopped = 1;
                    return AVERROR_EOF;
                }
                else {
                    if (cell_event->pgN != parts[cur_chapter_index]) {
                        cur_chapter_index++;
                        if (nb_of_chapter <= cur_chapter_index) {
                            stopped = true;
                            return AVERROR_EOF;
                        }
                        dvdnav_reset(priv->dvdnav);
                        if (dvdnav_part_play(priv->dvdnav, priv->title, parts[cur_chapter_index]) != DVDNAV_STATUS_OK) {
                            qDebug("dvdnav_part_play: %s\n", dvdnav_err_to_string(priv->dvdnav));
                            stopped = 1;
                            return AVERROR_EOF;
                        }
                    }
                }
            }
            break;
        }
        case DVDNAV_STOP:
            stopped = true;
            qDebug("dvdnav: stop\n");
            return AVERROR_EOF;
        default:
            break;
        }
    }
    return 0;
}
#endif

bool DVDNavIOPrivate::stream_dvdnav_seek(stream_t *s, int64_t newpos)
{
    uint32_t sector = 0;

    if (s->end_pos && newpos > s->end_pos)
        newpos = s->end_pos;
    sector = newpos / 2048ULL;
    if (dvdnav_sector_search(priv->dvdnav, (uint64_t)sector, SEEK_SET) != DVDNAV_STATUS_OK)
        return false;
    s->pos = newpos;
    return true;
}

// 对DVD读取的一个包装，屏蔽了大部分不需要的事件。
int DVDNavIOPrivate::get_block(uint8_t *buf, int *event) 
{
    int len = 0, error_count = 0;

    while (!stopped) {
        if (dvdnav_get_next_block(priv->dvdnav, buf, event, &len) == DVDNAV_STATUS_ERR) {
            if (dvdnav_sector_search(priv->dvdnav, 1, SEEK_CUR) == DVDNAV_STATUS_ERR) {
                qDebug("sector search error : %s\n", dvdnav_err_to_string(priv->dvdnav));
            }
            ++error_count;
            if (error_count > 500) {
                qDebug("Too many consecutive read errors!\n");
                return -1;
            }
        }
        switch (*event) {
        case DVDNAV_BLOCK_OK:
            return len;
            break;
        case DVDNAV_VTS_CHANGE: {
            dvdnav_vts_change_event_t *vts_change = (dvdnav_vts_change_event_t*)buf;
            qDebug("DVDNAV_VTS_CHANGE\n");
            qDebug("\told domain %d vts %d\n", vts_change->old_domain, vts_change->old_vtsN);
            qDebug("\tnew domain %d vts %d\n", vts_change->new_domain, vts_change->new_vtsN);
            return len;
            break;
        }
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t *cell_change = (dvdnav_cell_change_event_t*)buf;
            qDebug("DVDNAV_CELL_CHANGE\n");
            qDebug("\tcellN=%d\n", cell_change->cellN);
            qDebug("\tpgN=%d\n", cell_change->pgN);
            qDebug("\tcell_start=%lld\n", cell_change->cell_start);
            qDebug("\tcell_len=%lld\n", cell_change->cell_length);
            qDebug("\tpg_start=%lld\n", cell_change->pg_start);
            qDebug("\tpg_len=%lld\n", cell_change->pg_length);
            qDebug("\tpgc_len=%lld\n", cell_change->pgc_length);
            return len;
            break;
        }
        case DVDNAV_NAV_PACKET: {
            pci_t *pci;
            if (skip_nav_pack) {
                break;
            }
            pci = dvdnav_get_current_nav_pci(priv->dvdnav);
            dvdnav_get_current_nav_dsi(priv->dvdnav);

            if (pci->hli.hl_gi.btn_ns > 0) {
                int button;
                qDebug("Found %i DVD menu buttons...\n", pci->hli.hl_gi.btn_ns);

                for (button = 0; button < pci->hli.hl_gi.btn_ns; button++) {
                    btni_t *btni = &(pci->hli.btnit[button]);
                    qDebug("Button %i top-left @ (%i,%i), bottom-right @ (%i,%i)\n",
                        button + 1, btni->x_start, btni->y_start,
                        btni->x_end, btni->y_end);
                }

                button = 0;
                while ((button <= 0) || (button > pci->hli.hl_gi.btn_ns)) {
                    qDebug("Which button (1 to %i): ", pci->hli.hl_gi.btn_ns);
                    scanf("%i", &button);
                    if (button == 100) {
                        stopped = true;
                        button = 0;
                        break;
                    }
                    else if (button == 200) {
                        skip_nav_pack = true;
                        button = 0;
                        break;
                    }
                }

                qDebug("Selecting button %i...\n", button);
                if (button) {
                    dvdnav_button_select_and_activate(priv->dvdnav, pci, button);
                }
            }
            break;
        }
        case DVDNAV_NOP:
            qDebug("DVDNAV_NOP\n");
            break;
        case DVDNAV_SPU_CLUT_CHANGE: {
            int i = 0;
            qDebug("DVDNAV_SPU_CLUT_CHANGE\n");
            for (i = 0; i < 0x1f; i++) {
                uint16_t lang = dvdnav_spu_stream_to_lang(priv->dvdnav, i);
                if (lang != 0xffff) {
                    qDebug("\tlange id %x %c%c\n", i + 0xdb20, (lang >> 8) & 0x0f, lang & 0x0f);
                }
            }
            break;
        }
        case DVDNAV_SPU_STREAM_CHANGE: {
            int i = 0;
            qDebug("DVDNAV_SPU_STREAM_CHANGE\n");
            for (i = 0; i < 0x1f; i++) {
                uint16_t lang = dvdnav_spu_stream_to_lang(priv->dvdnav, i);
                if (lang != 0xffff) {
                    qDebug("\tlange id %x %c%c\n", i + 0xdb20, (lang >> 8) & 0x0f, lang & 0x0f);
                }
            }
            break;
        }
        case DVDNAV_AUDIO_STREAM_CHANGE: {
            dvdnav_audio_stream_change_event_t* audio_change = (dvdnav_audio_stream_change_event_t*)buf;
            qDebug("DVDNAV_AUDIO_STREAM_CHANGE\n");
            qDebug("\tphysical=%d\n", audio_change->physical);
            break;
        }
        case DVDNAV_HIGHLIGHT: {
            dvdnav_highlight_event_t *highlight_event = (dvdnav_highlight_event_t *)buf;
            qDebug("DVDNAV_HIGHLIGHT\n");
            qDebug("\tSelected button %d\n", highlight_event->buttonN);
            break;
        }
        case DVDNAV_HOP_CHANNEL:
            qDebug("DVDNAV_HOP_CHANNEL\n");
            break;
        case DVDNAV_STILL_FRAME: {
            pci_t *pci;
            dvdnav_still_event_t *still_event = (dvdnav_still_event_t *)buf;
            qDebug("DVDNAV_STILL_FRAME\n");
            if (still_event->length < 0xff)
                qDebug("\tSkipping %d seconds of still frame\n", still_event->length);
            else
                qDebug("\tSkipping indefinite length still frame\n");
            pci = dvdnav_get_current_nav_pci(priv->dvdnav);
            dvdnav_button_activate(priv->dvdnav, pci);
            dvdnav_still_skip(priv->dvdnav);
            break;
        }
        case DVDNAV_WAIT: {
            qDebug("DVDNAV_WAIT\n");
            dvdnav_wait_skip(priv->dvdnav);
            break;
        }
        case DVDNAV_STOP:
            qDebug("DVDNAV_STOP\n");
            stopped = true;
            break;
        }
    }
    return -1;
}

int DVDNavIOPrivate::dvdnav_first_play()
{
    current_chapter_index = 0;
    if (current_title > 0) {
//        if (parts.isEmpty()) {
            if (dvdnav_title_play(priv->dvdnav, current_title) != DVDNAV_STATUS_OK) {
                qDebug("dvdnav_stream, couldn't select title %d, error '%s'\n", current_title, dvdnav_err_to_string(priv->dvdnav));
                stream_dvdnav_close();
                dvd_stream = NULL;
                return STREAM_UNSUPPORTED;
            }
            qDebug("ID_DVD_CURRENT_TITLE=%d\n", current_title);
//        }
//        else {
//            current_chapter = parts[current_chapter_index];
//            if (dvdnav_part_play(priv->dvdnav, current_title, current_chapter) != DVDNAV_STATUS_OK) {
//                qDebug("dvdnav_stream, couldn't select title %d, part %d, error '%s'\n", current_title, current_chapter, dvdnav_err_to_string(priv->dvdnav));
//                stream_dvdnav_close(dvd_stream);
//                dvd_stream = NULL;
//                return STREAM_UNSUPPORTED;
//            }
//        }
    }
    else if (current_title == 0) {
        if (dvdnav_menu_call(priv->dvdnav, DVD_MENU_Root) != DVDNAV_STATUS_OK)
            dvdnav_menu_call(priv->dvdnav, DVD_MENU_Title);
    }
    return STREAM_OK;
}

void DVDNavIOPrivate::update_title_len(stream_t *s)
{
    dvdnav_status_t status;
    uint32_t pos = 0, len = 0;

    status = dvdnav_get_position(priv->dvdnav, &pos, &len);
    if (status == DVDNAV_STATUS_OK && len) {
        s->end_pos = len * 2048ull;
    }
    else {
        s->end_pos = 0;
    }
}

int64_t DVDNavIOPrivate::update_current_time()
{
    int64_t time = dvdnav_get_current_time(priv->dvdnav) / 90.0f;
    time -= current_start_time;
    if (time != current_time) {
        current_time = time;
    }
    if (!started) {
        current_start_time = time;
        started = true;
    }
    return current_time;
}

#ifdef _WIN32
static int utf8_to_mb(char* str_utf8, char* local_buf, size_t len) {
    int widelen;
    wchar_t *widestr;
    int mblen;

    widelen = MultiByteToWideChar(CP_UTF8, 0, str_utf8, strlen(str_utf8), NULL, 0);
    widestr = (wchar_t *)malloc(sizeof(wchar_t)*(widelen + 1));
    if (widestr == NULL) {
        return 0;
    }
    MultiByteToWideChar(CP_UTF8, 0, str_utf8, strlen(str_utf8), widestr, widelen);

    mblen = WideCharToMultiByte(CP_ACP, 0, widestr, widelen, NULL, 0, NULL, NULL);
    if (mblen + 1 > len) {
        free(widestr);
        return 0;
    }
    WideCharToMultiByte(CP_ACP, 0, widestr, widelen, local_buf, mblen, NULL, NULL);
    local_buf[mblen] = '\0';
    free(widestr);
    return 1;
}
#endif

int DVDNavIOPrivate::stream_dvdnav_open(const char *file)
{
    struct stream_nav_priv_s* p = NULL;
    dvdnav_status_t status = DVDNAV_STATUS_ERR;
    const char *titleName;
    int titles = 0;
    char filename[2048];
    char *temp = strdup(file);
    const char* filesystem[] = { "auto", "udf", "iso9660" };

    clock_flag = 2;
    memset(filename, 0, 2048);
#ifdef _WIN32
    if (!utf8_to_mb(temp, filename, 2048)) {
        free(temp);
        return STREAM_UNSUPPORTED;
    }
#endif
    free(temp);
    if (dvd_stream == NULL) {
        dvd_stream = new_stream(-2, -2);
        dvd_stream->flags |= STREAM_READ;
    }

    if (!(priv = (dvdnav_priv_t*)calloc(1, sizeof(dvdnav_priv_t))))
        return STREAM_UNSUPPORTED;

    DVDReadMode(file_sys);
    if (dvdnav_open(&(priv->dvdnav), filename) != DVDNAV_STATUS_OK || !priv->dvdnav) {
        delete dvd_stream;
        dvd_stream = NULL;
        delete priv;
        priv = NULL;
        return STREAM_UNSUPPORTED;
    }
    if (1) {	//from vlc: if not used dvdnav from cvs will fail
        int len, event;
        uint8_t buf[2048];
        dvdnav_get_next_block(priv->dvdnav, buf, &event, &len);
        dvdnav_sector_search(priv->dvdnav, 0, SEEK_SET);
    }
    /* turn off dvdnav caching */
    dvdnav_set_readahead_flag(priv->dvdnav, 0);
    if (dvdnav_set_PGC_positioning_flag(priv->dvdnav, 1) != DVDNAV_STATUS_OK)
        qDebug("stream_dvdnav, failed to set PGC positioning\n");

    /* report the title?! */
    if (dvdnav_get_title_string(priv->dvdnav, &titleName) == DVDNAV_STATUS_OK) {
        qDebug("ID_DVD_VOLUME_ID=%s\n", titleName);
    }

    status = dvdnav_get_number_of_titles(priv->dvdnav, &titles);
    if (status != DVDNAV_STATUS_OK || current_title > titles) {
        current_title = 0;
    }

    if (!dvdnav_first_play()) {
        return STREAM_UNSUPPORTED;
    }

    if(current_title > 0)
        show_audio_subs_languages(priv->dvdnav);
    if(dvd_angle > 1)
        dvdnav_angle_change(priv->dvdnav, dvd_angle);

    dvd_stream->sector_size = 2048;
    dvd_stream->flags = STREAM_READ;
    dvd_stream->type = STREAMTYPE_DVDNAV;

    {
        char buf[STREAM_BUFFER_SIZE];
        int ret = 0;
        while (dvd_stream->end_pos == 0) {
            ret = stream_dvdnav_read(dvd_stream, buf, STREAM_BUFFER_SIZE);
            if (ret <= 0) {
                /*Prevent forever loops if the DVD file is invalid.*/
                break;
            }
        }
        if (!dvdnav_first_play()) {
            return STREAM_UNSUPPORTED;
        }
    }
    //stream_dvdnav_seek(dvd_stream, dvd_stream->start_pos);
    if(!dvd_stream->pos && current_title > 0)
        qDebug() << "INIT ERROR: couldn't get init pos %s\r\n" << dvdnav_err_to_string(priv->dvdnav);
    stream_dvdnav_get_time();

    return STREAM_OK;
}

void DVDNavIOPrivate::stream_dvdnav_close()
{
    if (dvd_stream) {
        delete dvd_stream;
        dvd_stream = NULL;
    }
    if (priv) {
        if (priv->dvdnav) dvdnav_close(priv->dvdnav);
        delete priv;
        priv = NULL;
    }
    current_title = 0;
    started = false;
    current_start_time = 0;
    current_time = 0;
    current_duration = 0;
    custom_duration = 0;
    current_cell = 0;
}

void DVDNavIOPrivate::stream_dvdnav_get_time()
{
    uint64_t *parts = NULL, duration = 0;
    uint32_t n, i;
    n = dvdnav_describe_title_chapters(priv->dvdnav, current_title, &parts, &duration);
    if (parts) {
        current_duration = duration / 90;
        current_duration *= 1000;
    }
}

} //namespace QtAV
#include "DVDNavIO.moc"
