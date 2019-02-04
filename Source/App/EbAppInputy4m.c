/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#include "EbAppInputy4m.h"
#define YFM_HEADER_MAX 80
#define YUV4MPEG2_IND_SIZE 9
#define PRINT_HEADER 0

/* reads the y4m header and parses the input parameters */
int32_t readY4mHeader(EbConfig_t *cfg){

    FILE *ptr_in;
    unsigned char buffer[YFM_HEADER_MAX];
    unsigned char *tokstart, *tokend;
    uint32_t bitdepth = 8, width = 0, height = 0, fr_n = 0,
        fr_d = 0, aspect_n, aspect_d;
    char chroma[4] = "420", scan_type = 'p';

    /* pointer to the input file */
    ptr_in = cfg->inputFile;

    /* get first line after YUV4MPEG2 */
    fgets((char*)buffer, sizeof(buffer), ptr_in);

    /* print header */
    if(PRINT_HEADER) {
        printf("y4m header:");
        fputs((const char *) buffer, stdout);
    }

    /* read header parameters */
    for (tokstart = &(buffer[0]); *tokstart != '\0'; tokstart++) {
        if (*tokstart == 0x20)
            continue;
        switch (*tokstart++) {
        case 'W': /* width, required. */
            width = strtol(tokstart, &tokend, 10);
            if(PRINT_HEADER)
                printf("width = %d\n", width);
            tokstart = tokend;
            break;
        case 'H': /* height, required. */
            height = strtol(tokstart, &tokend, 10);
            if(PRINT_HEADER)
                printf("height = %d\n", height);
            tokstart = tokend;
            break;
        case 'I': /* scan type, not required, default: 'p' */
            switch (*tokstart++) {
            case '?':
                scan_type = '?';
                break;
            case 'p':
                scan_type = 'p';
                break;
            case 't':
                scan_type = 't';
                break;
            case 'b':
                scan_type = 'b';
                break;
            default:
                fprintf(cfg->errorLogFile, "Interlace type not supported\n");
                return EB_ErrorBadParameter;
            }
            if(PRINT_HEADER)
                printf("scan_type = %c\n", scan_type);
            break;
        case 'C': /* color space, not required: default "420" */
            if (strncmp("420p16", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "420");
                bitdepth = 16;
            } else if (strncmp("422p16", (const char*)tokstart, 6) == 0) {
                 strcpy(chroma, "422");
                 bitdepth = 16;
            } else if (strncmp("444p16", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "444");
                bitdepth = 16;
            } else if (strncmp("420p14", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "420");
                bitdepth = 14;
            } else if (strncmp("422p14", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "422");
                bitdepth = 14;
            } else if (strncmp("444p14", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "444");
                bitdepth = 14;
            } else if (strncmp("420p12", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "420");
                bitdepth = 12;
            } else if (strncmp("422p12", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "422");
                bitdepth = 12;
            } else if (strncmp("444p12", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "444");
                bitdepth = 12;
            } else if (strncmp("420p10", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "420");
                bitdepth = 10;
            } else if (strncmp("422p10", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "422");
                bitdepth = 10;
            } else if (strncmp("444p10", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "444");
                bitdepth = 10;
            } else if (strncmp("420p9", (const char*)tokstart, 5) == 0) {
                strcpy(chroma, "420");
                bitdepth = 9;
            } else if (strncmp("422p9", (const char*)tokstart, 5) == 0) {
                strcpy(chroma, "422");
                bitdepth = 9;
            } else if (strncmp("444p9", (const char*)tokstart, 5) == 0) {
                strcpy(chroma, "444");
                bitdepth = 9;
            } else if (strncmp("420", (const char*)tokstart, 3) == 0) {
                strcpy(chroma, "420");
                bitdepth = 8;
            } else if (strncmp("411", (const char*)tokstart, 3) == 0) {
                strcpy(chroma, "411");
                bitdepth = 8;
            } else if (strncmp("422", (const char*)tokstart, 3) == 0) {
                strcpy(chroma, "422");
                bitdepth = 8;
            } else if (strncmp("mono16", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "400");
                bitdepth = 16;
            } else if (strncmp("mono12", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "400");
                bitdepth = 12;
            } else if (strncmp("mono10", (const char*)tokstart, 6) == 0) {
                strcpy(chroma, "400");
                bitdepth = 10;
            } else if (strncmp("mono9", (const char*)tokstart, 5) == 0) {
                strcpy(chroma, "400");
                bitdepth = 9;
            } else if (strncmp("mono", (const char*)tokstart, 4) == 0) {
                strcpy(chroma, "400");
                bitdepth = 8;
            } else {
                fprintf(cfg->errorLogFile, "chroma format not supported\n");
                return EB_ErrorBadParameter;
            }
            while (*tokstart != '\n' && *tokstart != 0x20)
                tokstart++;
            if(PRINT_HEADER)
                printf("chroma = %s, bitdepth = %d\n", chroma, bitdepth);
            break;
        case 'F': /* frame rate, required */
            sscanf((const char*)tokstart, "%d:%d", &fr_n, &fr_d); // 0:0 if unknown
            if(PRINT_HEADER) {
                printf("framerate_n = %d\n", fr_n);
                printf("framerate_d = %d\n", fr_d);
            }
            while (*tokstart != '\n' && *tokstart != 0x20) {
                tokstart++;
            }
            break;
        case 'A': /* aspect ratio, not required */
            sscanf((const char*)tokstart, "%d:%d", &aspect_n, &aspect_d); // 0:0 if unknown
            if(PRINT_HEADER) {
                printf("aspect_n = %d\n", aspect_n);
                printf("aspect_d = %d\n", aspect_d);
            }
            while (*tokstart != '\n' && *tokstart != 0x20) {
                tokstart++;
            }
            break;
        }
    }

    /*check if required parameters were read*/
    if(width == 0) {
        fprintf(cfg->errorLogFile, "width not found in y4m header\n");
        return EB_ErrorBadParameter;
    }
    if(height == 0) {
        fprintf(cfg->errorLogFile, "height not found in y4m header\n");
        return EB_ErrorBadParameter;
    }
    if(fr_n == 0 || fr_d == 0) {
        fprintf(cfg->errorLogFile, "frame rate not found in y4m header\n");
        return EB_ErrorBadParameter;
    }

    /* read next line with contains "FRAME" */
    fgets((char*)buffer, sizeof(buffer), ptr_in);

    /* Assign parameters to cfg */
    cfg->sourceWidth = width;
    cfg->sourceHeight = height;
    cfg->frameRateNumerator = fr_n;
    cfg->frameRateDenominator = fr_d;
    cfg->frameRate = fr_n/fr_d;
    cfg->encoderBitDepth = bitdepth;
    /* TODO: when implemented, need to set input bit depth
        (instead of the encoder bit depth) and chroma format */

    return EB_ErrorNone;

}

/* read next line which contains the "FRAME" delimiter */
int32_t readY4mFrameDelimiter(EbConfig_t *cfg){

    unsigned char bufferY4Mheader[10];

    fgets((char *)bufferY4Mheader, sizeof(bufferY4Mheader), cfg->inputFile);

    if (strcmp((const char*)bufferY4Mheader, "FRAME\n") != 0) {
        fprintf(cfg->errorLogFile, "Failed to read propor y4m frame delimeter. Read broken.\n");
        return EB_ErrorBadParameter;
    }

    return EB_ErrorNone;

}

/* check if the input file is in YUV4MPEG2 (y4m) format */
EbBool checkIfY4m(EbConfig_t *cfg){

    unsigned char buffer[YUV4MPEG2_IND_SIZE+1];

    /* Parse the header for the "YUV4MPEG2" string */
    fread(buffer, YUV4MPEG2_IND_SIZE, 1, cfg->inputFile);
    buffer[YUV4MPEG2_IND_SIZE] = 0;

    if (strcmp((const char*)buffer, "YUV4MPEG2") == 0) {
        return EB_TRUE; /* YUV4MPEG2 file */
    }else{
        if(cfg->inputFile != stdin) {
            fseek(cfg->inputFile, 0, SEEK_SET);
        }else{
            memcpy(cfg->y4mBuf, buffer, YUV4MPEG2_IND_SIZE); /* TODO copy 9 bytes read to cfg->y4mBuf*/
        }
        return EB_FALSE; /* Not a YUV4MPEG2 file */
    }

}