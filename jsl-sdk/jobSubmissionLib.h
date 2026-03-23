#ifndef JOBSUBMISSIONLIB_H_10241324022016
#define JOBSUBMISSIONLIB_H_10241324022016
/**
 * @file
 *
 *   Interface for supplying Print Data to Memjet printers that are based on the Glenbeigh
 *   print pipeline.
 */

/*
 * (C) 2016 Memjet Ltd. All content is the confidential property of, or licensed
 * to, Memjet Ltd. ("Memjet," "we," or "us") and is protected under U.S. and
 * Foreign copyright, trademark and other intellectual property laws.
 *
 * Portions of this code may contain copyrighted materials from Memjet's
 * suppliers, and copying and distribution of such materials for any other
 * purpose except as permitted under a written agreement with respect to the
 * Memjet Technology is expressly prohibited.
 */

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Define Windows-specific macros.
 * On other platforms these macros do nothing.
 */
#ifdef _WINDOWS
    /*
     * Define the DLL_API macro so that:
     * - entrypoints will be exported by builds of the library itself
     * - entrypoints will be imported by users of the library
     */
    #ifdef JOBSUBMISSIONLIB_EXPORTS
    #define DLL_API __declspec( dllexport )
    #else
    #define DLL_API __declspec( dllimport )
    #endif

    /*
     * This API relies on the CALLBACK macro, which must therefore be defined
     * before this file is included.  However, the preferred way to include the
     * macro definition depends on the user's application.  The typical approach
     * is to include windows.h, and perhaps define WIN32_LEAN_AND_MEAN before
     * doing so, however other techniques may be needed to avoid clashes between
     * the windows headers and other headers.  Therefore, in order to not create
     * any clashes for the user, we do not include any windows headers here, and
     * instead require that the user does so before including this file.
     */
    #ifndef CALLBACK
    #error CALLBACK macro not defined.  Please include the windows headers before including this file.
    #endif
#else
    #define DLL_API
    #define CALLBACK
#endif

/**
 * @section jslibIntro JobSubmissionLib Introduction
 *
 * JobSubmissionLib allows a RIP to supply print data for a single printhead in a Memjet
 * printer based on the Glenbeigh print pipeline. It assembles a job's print data and
 * associated metadata into the format described in the 7-213-1-1 Glenbeigh Borealis File
 * Format document.
 *
 * A call to jslibOpenJob() provides job-level attributes, and returns a handle.
 * The handle represents a stream to which each page's print data is then passed by calls
 * to jslibAddPage(). Finally a call to jslibCloseJob() indicates that all the job's print
 * data has been added, and the handle is invalidated. For every jslibOpenJob() call
 * there should be a corresponding jslibCloseJob().
 *
 * If the caller is producing data for more than one printhead then it will call
 * jslibOpenJob() once for each printhead, thus producing a set of handles that can be
 * used concurrently. It will then add the data for each printhead to the corresponding
 * handle.
 *
 * There is also a log facility so that JobSubmissionLib can publish activity and error
 * details. Data can also be redirected to a file instead of a network connection.
 *
 * @section dataFormat Print Data Format
 *
 * Bilevel print data consists of a sequence of bits representing the dots in a single
 * color channel. The resolution of dots, horizontally and vertically, is set by parameters
 * passed to jslibOpenJob() - see @c xResolution and @c yResolutionMode.
 *
 * A bit with a value of 1 indicates that a dot is printed. Bits are grouped together as
 * 32-bit words, with the left-most dot in bit 0 and the right-most dot in bit 31.
 * This is the canonical data order. Variations are supported which may better match the
 * ordering available from the data source - see the @c endianness job parameter.
 *
 * There is an additional requirement for the overall length of each line of data. At the
 * end of a line of dots additional padding bits must be supplied with the data to ensure
 * that a line is stored in a whole number of 128-bit words.
 *
 * Note that the total width of the supplied print data (including padding) must be within
 * the maximum supported by the printer (consult your printer documentation) otherwise the
 * print job will fail to print.
 *
 * @section stripDef Strips
 *
 * The vertical section of a print job that is to be printed by a single printhead is
 * termed a strip. A printer with more than one printhead across the page will have the
 * Source Image divided into several strips, one per printhead.
 *
 * Strips can be thought of as printing on a single page-wide 'virtual printhead' made up
 * from the printer's physical printheads. Parameters passed to JobSubmissionLib in
 * JslibJobDescription describe the strip (notions of 'left' and 'right' are based on
 * looking at the printheads firing down, and paper moving away from the observer):
 *
 * - @c stripStart is the number of dots to the start of the strip from the engine stage datum.
 *   In an NxM context, this is the printableOffset converted to dots.
 * - @c stripWidth is the number of dots of Source Image contained in the strip
 *   excluding padding.
 *   In an NxM context, this is the printableWidth converted to dots. If the printableWidth
 *   of a Printhead Module is zero, then the stripWidth will also be zero, but the
 *   RIP must generate a stream of null data (zeros padded out to 128-dots) for that strip.
 *   See the "Duralink Job Submission Library Guide".
 *
 * @section interleave Sending interleaved print data
 *
 * The Glenbeigh Borealis File Format supports sending interleaved print data to
 * printhead modules (PHMs). The printer systems that support interleaved printing
 * can be plumbed for either monochrome or color printing. The setting,
 * JslibJobDescription::lineInterleaveSize is used to specify the number of the
 * same color print data channels that are interleaved on the printed page to give the
 * page height specified in JslibPageDescription::pageHeight. Set
 * JslibJobDescription::lineInterleaveSize to one to indicate that interleaving feature
 * is not in use.
 *
 * When interleaving is enabled (i.e JslibJobDescription::lineInterleaveSize > 1), each
 * entry in JslibPageDescription::colorArray must have a corresponding entry in
 * JslibJobDescription::lineInterleaveIndex array that specifies the interleave index
 * for the color channel. If a JslibColor appears more than once in
 * JslibPageDescription::colorArray, then each entry must be assigned a unique
 * lineInterleaveIndex value less than JslibJobDescription::lineInterleaveSize.
 * When interleaving is disabled (i.e JslibJobDescription::lineInterleaveSize == 1),
 * JslibJobDescription::lineInterleaveIndex parameter is not applicable and must be set to NULL.
 *
 * When printing with interleaving enabled, JslibPageDescription::dataPtr array contains
 * pointers to interleaved data corresponding to the line interleave indices specified in
 * JslibJobDescription::lineInterleaveIndex array. The interleaved print data size
 * (JslibPageDescription::len) corresponding to different indices of the same color must
 * be set to the same value to produce the same number of lines on the printed page.
 *
 * The following example shows how to populate the elements that affect interleaving in
 * JslibJobDescription structure in order to send a job to a PHM plumbed with two colors
 * (4 color channels) in a tandem print system.
 *     numColors  = 4
 *     colorArray = [JSLIB_CYAN, JSLIB_BLACK, JSLIB_BLACK, JSLIB_CYAN]
 *     lineInterleaveSize = 2
 *     lineInterleaveIndex = [0, 0, 1, 1]
 *
 * In the above example, a lineInterleaveIndex values of 0 and 1 for a given color corresponds
 * to the print data that get consumed (JslibPageDescription::dataPtr) when printing
 * even (lineInterleaveIndex = 0) and odd (lineInterleaveIndex = 1) lines on the printed page.
 */

/**
 * The colors for which print data may be supplied.
 * @note Color enums start at one. Zero is not a valid color.
 */
typedef enum JslibColor
{
    JSLIB_CYAN = 1,
    JSLIB_MAGENTA,
    JSLIB_YELLOW,
    JSLIB_BLACK,
    JSLIB_ORANGE,
    JSLIB_GREEN,
    JSLIB_VIOLET,
    JSLIB_SPOT,
    JSLIB_SPOT2
} JslibColor;

/**
 * The maximum number of colors that can be supplied to an individual printhead.
 */
#define JSLIB_MAX_NUM_COLORS_PER_PRINTHEAD 5

/**
 * Number of characters in the hex representation of a Job Id.
 */
#define JSLIB_LEN_JOB_ID 32

/**
 * The level of detail to be sent to the logging callback.
 * These are in order of priority.
 */
typedef enum JslibLoggingLevel
{
    /// Suppress all output.
    JSLIB_LOGGING_LEVEL_NONE = 0,

    /// Only report errors.
    JSLIB_LOGGING_LEVEL_ERROR,

    /// Report errors and warnings.
    JSLIB_LOGGING_LEVEL_WARNING,

    /// Trace library activity and report warnings and errors. This is the default.
    JSLIB_LOGGING_LEVEL_INFO,

    /**
     * As for INFO with additional detail probably of interest only to Job Submission Library
     * developers.
     */
    JSLIB_LOGGING_LEVEL_DEBUG
} JslibLoggingLevel;

/**
 * Set the level of logging output reported by the logging callback. Only messages with
 * logging level <= current logging level will be passed back to the logging callback.
 *
 * @param loggingLevel the desired logging level.
 */
DLL_API void jslibSetLoggingLevel(JslibLoggingLevel loggingLevel);

/**
 * Defines the type for the logging callback function.
 */
typedef void (CALLBACK *JslibLoggingCallback)(JslibLoggingLevel loggingLevel,
                                              const char *text);

/**
 * Sets the function to be called when text is logged from the Job Submission Library.
 *
 * Only one callback function is maintained. Each call to jslibSetLoggingCallback replaces
 * the function specified in a previous call. To stop receiving callbacks, specify
 * NULL as the callback function.
 *
 * @note The callback function must not throw any exception and must not block.
 *
 * Each time this function is called with a non-zero value, a JSLIB_LOGGING_LEVEL_INFO
 * log message will be added which indicates the version of the Job Submission Library.
 * Like all log messages, this is subject to filtering, so if jslibSetLoggingLevel()
 * has been called with a value higher than INFO before this function is called, then
 * this version message will not be seen.
 *
 * @param callback  The function to be called when data is logged by the Job Submission
 *                  Library. The callback function may run either in a client thread
 *                  or in a Job Submission Library internal thread. In any case, it
 *                  must not throw any exception and must not block.
 */
DLL_API void jslibSetLoggingCallback(JslibLoggingCallback callback);

/**
 * The result returned by JobSubmissionLib functions.
 */
typedef enum JslibResultCode
{
    /** Successful completion */
    JSLIB_RETURN_CODE_OK,

    /**
     * The print engine is not ready to print or the library is not
     * ready to receive this call. See log output for more information.
     */
    JSLIB_RETURN_CODE_NOT_READY,

    /**
     * The destination for this job was unable to be resolved.
     */
    JSLIB_RETURN_CODE_RESOLVE_DEST,

    /**
     * A communications error was encountered.
     */
    JSLIB_RETURN_CODE_COMMS,

    /**
     * The connection was closed from the other end.
     */
    JSLIB_RETURN_CODE_CLOSED,

    /**
     * An invalid parameter was supplied.
     * See log output for more information.
     */
    JSLIB_RETURN_CODE_BAD_PARAM,

    /**
     * The resource is currently in use.
     */
    JSLIB_RETURN_CODE_BUSY,

    /**
     * Current operation was aborted by calling jslibAbortJob().
     */
    JSLIB_RETURN_CODE_ABORT,

    /**
     * An unexpected error was encountered. After receiving this return code,
     * the library may not be in a consistent state and should be shut down.
     * All functions on this interface may return this value at any time.
     * See log output for more information.
     */
    JSLIB_RETURN_CODE_FATAL_ERROR

} JslibResultCode;

/**
 * An element of arbitrary data that can optionally be associated with a job.
 */
typedef struct JslibCustomData
{
    /**
     * A number that identifies the associated data.
     */
    uint32_t tag;

    /**
     * The length, in bytes, of the data.
     */
    uint32_t length;

    /**
     * The location of the data.
     *
     * The data will be copied during the jslibOpenJob() call, so ownership of the data
     * stays with the caller.
     */
    uint8_t *data;
} JslibCustomData;

/**
 * Collection of attributes that describe the portion of a print job to be sent to a
 * printhead.
 */
typedef struct JslibJobDescription
{
    /**
     * The engine stage for this printhead.
     *
     * Stages are numbered from one. Typical values:
     * For a simplex printer use 1; for a duplex printer: 1 is the front
     * side of the media, 2 is the back side.
     */
    uint32_t engineStage;

    /**
     * The strip index for this printhead.
     *
     * Strips are numbered from left-to-right, when viewing the output with the
     * printhead(s) firing down, and paper moving away from the observer,
     * starting from one.
     */
    uint32_t stripIndex;

    /**
     * The number of entries in the @c colorArray parameter.
     *
     * Cannot exceed @c JSLIB_MAX_NUM_COLORS_PER_PRINTHEAD.
     */
     uint32_t numColors;

    /**
     * An array of all the colors whose data will be sent to the
     * printhead, via later jslibAddPage() calls.  If more than one
     * instance of a color occurs in the array, then the instance
     * number of each successive repetition will increment by 1.
     *
     * For example, if the array contains the following elements:
     * JSLIB_BLACK, JSLIB_SPOT, JSLIB_BLACK, JSLIB_SPOT, JSLIB_SPOT
     * then the corresponding channel ink names (with instance
     * numbers) for the 5 channels will be:
     * BLACK@1, SPOT@1, BLACK@2, SPOT@2, SPOT@3
     *
     * The channel color instance is never explicitly provided in a
     * colorArray entry.  Whenever numColors > 1, the channel
     * color instance is determined implicitly by the entries in the
     * colorArray.  The separate printhead module color
     * instance is determined explicitly by the colorInstance
     * parameter.
     */
    JslibColor *colorArray;

    /**
     * The printhead module instance number of the colorArray.
     *
     * Commonly, this value is '1'.  However, where multiple printheads
     * within a strip on the same engine stage share the same color set,
     * this one-based instance number uniquely identifies the printheads.
     * For example, if there are two 'black' printhead modules within
     * one strip on the same engine stage, then their instance numbers
     * would be '1' and '2'.
     *
     * @note The colorInstance may differ from '1' only for mono
     *       printheads and only when the color array for different
     *       printbars on the same stage has identical entries.  In
     *       the case of one printhead plumbed for SPOT and another
     *       for SPOT2, the color entries are not identical, so both
     *       would have instance '1'.
     *       If numColors > 1, the colorInstance is always 1.
     */
    uint32_t colorInstance;

    /**
     * The total number of interleaved lines per ink color channel that
     * constitute a completed page on media. Set this to 1 when interleaving
     * is not in use.
     */
    uint32_t lineInterleaveSize;

    /**
     * When interleaving is enabled (i.e JslibJobDescription::lineInterleaveSize > 1), this
     * points to an uint32_t array of JslibJobDescription::numColors size. Each entry
     * contains the interleave index assigned to the corresponding color channel in the
     * JslibJobDescription::colorArray parameter.
     *
     *   0 <= lineInterleaveIndex[channel] < lineInterleaveSize
     *
     * When interleaving is disabled this parameter is not in use and must be
     * set to NULL.
     */
    uint32_t *lineInterleaveIndex;

    /**
     * Total number of pages to be printed in the job, including all multiple
     * copies of pages (see JslibPageDescription::numCopies).
     *
     * This is an optional field - set to zero if the number of pages is not known
     * when jslibOpenJob() is called. All files/streams for the same job must
     * supply an identical pageCount.
     */
    uint32_t pageCount;

    /**
     * The offset in dots to the start of the strip from the engine stage datum.
     *
     * See the explanation in @ref stripDef.
     */
    uint32_t stripStart;

    /**
     * The width in dots of image data in the strip.
     *
     * Data must be supplied to all active printheads in the printer, including any
     * padding in the rightmost strip(s) beyond the end of the Source Image.
     *
     * Note that each line of print data may require additional padding - see @ref
     * dataFormat. That padding is not included in @c stripWidth.
     */
    uint32_t stripWidth;

    /**
     * Job Identifier.
     *
     * The format is determined by the OEM.
     *
     * All files/streams for the same job must supply an identical jobId. Note that it is
     * a byte array, not a null-terminated string, so comparisons between jobIds are
     * performed over the entire array length.
     *
     * Care should be exercised that jobIds are unique, or only reused over a long
     * period, to avoid confusion between a new job and one that was processed earlier but
     * may still be tracked in the printer control system.
     *
     * The job ID is represented in hexadecimal format, eg. "00af16d56a3432784cb59238ab33f029".
     * All characters in the job ID array should be defined and must contain only hexadecimal
     * characters ie ASCII characters 0-9, a-f or A-F. The most significant character is
     * stored in the first (lowest) address in the array.
     */
    char jobId[JSLIB_LEN_JOB_ID];

    /**
     * Endianness reordering to be applied to the print data.
     *
     * The canonical format of print data is described in @ref dataFormat.
     * This parameter allows additional re-ordering of the data to be specified, which may
     * allow data to be supplied in a more convenient native format, and to be transformed
     * to the canonical format later, by the print pipeline. The reordering facility can
     * affect the order of bits within a byte, and also the order of bytes within
     * 32-bit words. The following values are supported:
     *     0: No bit or byte reordering is done. Data will be supplied in canonical format.
     *     1: Bits within each 8-bit byte are swapped. Byte ordering is not affected.
     *     2: Bytes within each 32-bit word are swapped. Bit ordering within each byte is not affected.
     *     3: Bits within each 8-bit byte are swapped. Bytes within each 32-bit word are swapped.
     * All files/streams for the same job will typically supply identical endianness.
     */
    uint32_t endianness;

    /**
     * Associates a human-readable arbitrary job name with the job.
     *
     * A null-terminated byte string, up to 255 bytes long (excluding the terminating null),
     * using UTF-8 encoding. All files/streams for the same job must supply an identical
     * displayableJobName.
     *
     * This field is optional, supply NULL if not required.
     *
     * The data will be copied during the jslibOpenJob() call, so ownership of the data
     * stays with the caller.
     */
    char *displayableJobName;

    /**
     * An arbitrary string identifying the RIP version.
     *
     * A null-terminated string, up to 31 bytes long (excluding the terminating null),
     * using UTF-8 encoding. All files/streams for the same job must supply an identical
     * ripVersion.
     *
     * This field is optional, supply NULL if not required.
     *
     * The data will be copied during the jslibOpenJob() call, so ownership of the data
     * stays with the caller.
     */
    char *ripVersion;

    /**
     * The horizontal resolution of the job in dots per inch. Must be 1600.
     */
    uint32_t xResolution;

    /**
     * The vertical resolution of the job in dots per inch.
     * All files/streams for the same job must supply identical yResolution.
     *
     * This value combined with the printMode parameter of jslibInit() is used
     * to lookup printer vertical resolution parameters in the "JslConfigs.xml"
     * file that accompanies this library.
     */
    uint32_t yResolution;

    /**
     * Set true if this job is to be printed with horizontal alignment disabled.
     * This would be used, for instance, when printing Optical Density Compensation
     * (ODC) calibration charts. In normal use, it should be cleared to false.
     */
    bool isHorizontalAlignmentDisabled;

    /**
     * The number of elements in the @c customOemData array.
     * All files/streams for the same job will typically supply identical
     * numCustomOemDataElements.
     */
    uint32_t numCustomOemDataElements;

    /**
     * An array of custom data, for OEM use.
     *
     * The @c tag values must be between 199200 and 199999 inclusive. The
     * meaning of the tag is defined by the OEM.
     *
     * The printer system will just store the data as supplied, and make it
     * available to OEM software as part of job information. Optional, specify
     * NULL if no custom data is required. All files/streams for the same job
     * will typically supply identical customOemData.
     *
     * The data will be copied during the jslibOpenJob() call, so ownership of the data
     * stays with the caller.
     */
    JslibCustomData *customOemData;

    /**
     * The number of elements in the @c customMemjetData array.
     * All files/streams for the same job will typically supply identical
     * numCustomMemjetDataElements.
     */
    uint32_t numCustomMemjetDataElements;

    /**
     * An array of custom data, for Memjet use.
     *
     * Reserved for use by Memjet software, other software should set to NULL.
     * All files/streams for the same job will typically supply identical
     * customMemjetData.
     *
     * The data will be copied during the jslibOpenJob() call, so ownership of the data
     * stays with the caller.
     */
    JslibCustomData *customMemjetData;

} JslibJobDescription;


/**
 * Attributes that describe a page within a print job.
 */
typedef struct JslibPageDescription
{
    /**
     * The height of this page, in dot lines.
     */
    uint32_t pageHeight;

    /**
     * The length, in bytes, of print data supplied for each @c colorArray entry.
     *
     * Note that entries must correspond to the entries in the @c colorArray
     * parameter to jslibOpenJob().  For example, if @c colorArray contains
     * CYAN,MAGENTA,YELLOW,BLACK then @c len[2] contains the length of print
     * data for yellow for this page.  For a multi-color printhead with more
     * than one entry of the same color in @c colorArray, then the corresponding
     * entry in @c len is the length of print data on this page for the color
     * instance.  For example, if colorArray is SPOT,SPOT then corresponding
     * entries @c len[0] and @c len[1] refer to the length of print data for
     * SPOT@1 and SPOT@2, respectively.
     *
     * Each entry in @c len must be a multiple of 16 bytes.
     */
    uint32_t len[JSLIB_MAX_NUM_COLORS_PER_PRINTHEAD];

    /**
     * Pointers to the print data for each color.
     *
     * Note that entries must correspond to the entries in the @c colorArray
     * parameter to jslibOpenJob(). For example, if @c colorArray contains
     * CYAN,MAGENTA,YELLOW,BLACK then @c dataPtr[2] points to the print data for
     * yellow for this page.  For a multi-color printhead with more than one
     * entry of the same color in @c colorArray, then the corresponding entry in
     * @c dataPtr points to the print data on this page for one color instance.  For
     * example, if colorArray is SPOT,SPOT then corresponding entries @c dataPtr[0]
     * and @c dataPtr[1] point to the print data for SPOT@1 and SPOT@2,
     * respectively.
     *
     * For each color instance, print data for the whole page must be supplied
     * in one contiguous buffer.
     */
    void *dataPtr[JSLIB_MAX_NUM_COLORS_PER_PRINTHEAD];

    /**
     * The number of copies of this page to be printed. Must not be zero.
     * This number must be included in the total count of pages for the job if
     * it is set (see JslibJobDescription::pageCount).
     */
    uint32_t numCopies;

    /**
     * Set true if this is the last page in a job, false otherwise.
     */
    bool isLastPage;

    /**
     * The number of elements in the @c customOemData array.  For a multi-color
     * printhead, set to 0.
     */
    uint32_t numCustomOemDataElements;

    /**
     * An array of custom data, for OEM use.
     *
     * The @c tag values must be between 199200 and 199999 inclusive. The
     * meaning of the tag is defined by the OEM.
     *
     * The printer system will store the data as supplied and make it
     * available to OEM software as part of page information. Optional, specify
     * NULL if no custom data are required.  For a multi-color printhead,
     * specify NULL.
     *
     * The data will be copied during the jslibAddPage() call, so ownership of the data
     * stays with the caller.
     */
     JslibCustomData *customOemData;

    } JslibPageDescription;

/**
 * The opaque handle used by the JobSubmissionLib.
 */
typedef void *JslibHdl;

/**
 * Initialise the Job Submission Library.
 *
 * This function must be successfully called once only before calling the job-related
 * functions in this library - ie jslibOpenJob(), jslibAddPage(), jslibCloseJob() &
 * jslibAbortJob().
 *
 * The logging functions  - ie jslibSetLoggingCallback() and jslibSetLoggingLevel() -
 * may be called at any time.
 *
 * If it is required to reinitialise then jslibShutdown() must be called first.
 *
 * @param configFileName  The full path to the system configuration XML file.
 * @param printMode       The mode in which the printer will operate. This mode
 *                        will be defined by the Printer OEM in consultation with
 *                        Memjet.
 *
 * @retval JSLIB_RETURN_CODE_OK         The library was successfully initialised.
 * @retval JSLIB_RETURN_CODE_BAD_PARAM  The configuration file could not be read,
 *                                      contained errors or printMode was not listed.
 * @retval JSLIB_RETURN_CODE_BUSY       This function has already been successfully called.
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR  Unexpected fatal error. The library may be
 *                                        in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibInit(const char *configFileName,
                                  uint32_t printMode);

/**
 * Shut down the Job Submission Library.
 *
 * Calling this function does not affect logging which remains functional. If the library
 * is not already initialised, calling this function does nothing.
 *
 * @retval JSLIB_RETURN_CODE_OK         The library was successfully shut down.
 * @retval JSLIB_RETURN_CODE_BUSY       A job is currently open (@see jslibCloseJob()).
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR  Unexpected fatal error. The library may be
 *                                        in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibShutdown(void);

/**
 * Send the description of a job that is about to begin to either a specified printhead or
 * to a file.
 *
 * If successful, a handle is returned that can be used to then send page data in the same
 * stream.
 *
 * jslibOpenJob() will take a copy of any data still required after the call completes,
 * so, for example, the @c jobDescription structure and any data it points to will not be
 * referenced once the call completes.
 *
 * @note The contents in the jobDescription parameter specified in the jslibOpenJob()
 *       uniquely identifies a particular printhead in a printer. Only one jslibOpenJob()
 *       request can be active at any given time for a given printhead.
 *
 * @pre jslibInit()
 *
 * @param jobDescription the job attributes.
 * @param fileName       the filename that data should be written to. Specify NULL to
 *                       indicates that output data is to be sent to a printer instead of
 *                       being saved to a file.
 * @param jslibHdl       address into which a handle is returned if the operation is successful.
 *
 * @retval JSLIB_RETURN_CODE_OK             Successful completion. @c jslibHdl is returned.
 * @retval JSLIB_RETURN_CODE_BAD_PARAM      An invalid parameter was supplied.
 *                                          See log output for more information.
 * @retval JSLIB_RETURN_CODE_NOT_READY      Sending to a printer failed because the print
 *                                          engine is not ready to print or the library has
 *                                          not been initialised (@see jslibInit()).
 * @retval JSLIB_RETURN_CODE_RESOLVE_DEST   The destination for this job was not able
 *                                          to be resolved.
 * @retval JSLIB_RETURN_CODE_COMMS          A communications error was encountered.
 *                                          Retry when communications have been
 *                                          re-established.
 * @retval JSLIB_RETURN_CODE_BUSY           The printhead specified in the jobDescription
 *                                          parameter is currently in use.
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR    Unexpected fatal error. The library may be
 *                                          in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibOpenJob(const JslibJobDescription *jobDescription,
                                     const char *fileName,
                                     JslibHdl *jslibHdl);

/**
 * Add the description and print data of the next page in the job to the stream associated
 * with a handle.
 *
 * jslibAddPage() will take a copy of any data still required after the call completes,
 * so, for example, the @c pageDescription structure and any data it points to will not be
 * referenced once the call completes. The actual page print data will be copied directly
 * from the buffer given by @c dataPtr to its final destination during the call to
 * jslibAddPage().
 *
 * @note Under normal operation this will block until all the page data has been sent to
 *       the printer. If the receiver is stalled (eg the receiver's buffer is full and the
 *       printer is not printing), this call will block until the receiver resumes
 *       accepting data. A call to jslibAbortJob() can be made (on another thread)
 *       while a call to jslibAddPage() is stalled.
 *
 * @pre jslibInit()
 *
 * @param jslibHdl          the handle returned from jslibOpenJob()
 * @param pageDescription   the page attributes.
 *
 * @retval JSLIB_RETURN_CODE_OK             Successful completion.
 * @retval JSLIB_RETURN_CODE_BAD_PARAM      An invalid parameter was supplied. This
 *                                          includes supplying an invalid @c jslibHdl.
 *                                          See log output for more information.
 * @retval JSLIB_RETURN_CODE_COMMS          A communications error was encountered.
 *                                          jslibCloseJob() must be called.
 * @retval JSLIB_RETURN_CODE_CLOSED         The connection was closed by the print engine,
 *                                          generally indicating that the print engine has
 *                                          deliberately aborted the job. jslibCloseJob()
 *                                          must be called.
 * @retval JSLIB_RETURN_CODE_ABORT          jslibAbortJob() was called before or while
 *                                          processing the command. jslibCloseJob()
 *                                          must be called.
 * @retval JSLIB_RETURN_CODE_NOT_READY      This function was called again after the previous
 *                                          call to jslibAddPage() returned an error or the
 *                                          library was not initialised (@see jslibInit()).
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR    Unexpected fatal error. The library may be
 *                                          in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibAddPage(JslibHdl jslibHdl,
                                     const JslibPageDescription *pageDescription);

/**
 * Indicates that the handle is no longer in use.
 *
 * Under the normal operation this is called once all the print data for the job has
 * been supplied. This must also be called if jslibAddPage() returns an error or
 * after calling jslibAbortJob().
 *
 * After this call the handle is no longer valid.
 *
 * @param jslibHdl  a handle returned from jslibOpenJob()
 *
 * @retval JSLIB_RETURN_CODE_OK             Successful completion.
 * @retval JSLIB_RETURN_CODE_BAD_PARAM      The supplied @c jslibHdl is invalid.
 * @retval JSLIB_RETURN_CODE_NOT_READY      The library was not initialised, or a 'last' page has
 *                                          not been added (@see PageDescription::isLastPage) and
 *                                          closeJobWithoutLastPageDelayMs == 0 and jslibAbortJob() has
 *                                          not been called.
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR    Unexpected fatal error. The library may be
 *                                          in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibCloseJob(JslibHdl jslibHdl);

/**
 * Indicates that the job is to be aborted.
 *
 * After this call, the job must be closed by calling jslibCloseJob() before
 * calling any other functions in this API.
 * If the data stream is being sent to a printer, the job will be discarded; if it has
 * started printing then printing is aborted.
 *
 * @note This must be called from a different thread to cancel a jslibAddPage()
 *       operation that is currently in progress or blocked.
 *
 * If the data stream is being sent to a file, the file will be deleted.
 *
 * @param jslibHdl a handle returned from jslibOpenJob()
 *
 * @retval JSLIB_RETURN_CODE_OK             Successful completion.
 * @retval JSLIB_RETURN_CODE_BAD_PARAM      The supplied @c jslibHdl is invalid.
 * @retval JSLIB_RETURN_CODE_NOT_READY      This function was called when the library
 *                                          was not initialised (@see jslibInit()).
 * @retval JSLIB_RETURN_CODE_FATAL_ERROR    Unexpected fatal error. The library may be
 *                                          in an invalid state and should be shut down.
 */
DLL_API JslibResultCode jslibAbortJob(JslibHdl jslibHdl);

#ifdef __cplusplus
}
#endif

#endif
