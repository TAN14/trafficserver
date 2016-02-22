/*
 * flv_tag.cc
 *
 *  Created on: 2015年11月17日
 *      Author: xie
 */

#include "flv_tag.h"
#include "des.h"

static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf, int64_t length);
static const char * get_amf_type_string(byte type);

static double int2double(uint64_t i)
{
    union av_intfloat64 v;
    v.i = i;
    return v.f;
}

static uint64_t double2int(double f)
{
	union av_intfloat64 v;
	v.f = f;
	return v.i;
}

int FlvTag::process_tag(TSIOBufferReader readerp, bool complete) {
	int64_t avail, head_avail;
	int rc;

	avail = TSIOBufferReaderAvail(readerp);
	TSIOBufferCopy(tag_buffer, readerp, avail, 0);

	TSIOBufferReaderConsume(readerp, avail);

    if(video_type == FLV_VIDEO && !is_init_current_handler) {
    		this->current_handler = &FlvTag::process_initial_flv_header;
    		this->is_init_current_handler = true;
    }
	rc = (this->*current_handler)();
	if (rc == 0 && complete) {
		rc = -1;
	}

	if (rc) {
		head_avail = TSIOBufferReaderAvail(head_reader);
		content_length = (cl - tag_pos) + head_avail;
//		TSDebug(PLUGIN_NAME, " content_length = %ld, discard_size=%lu ",content_length, duration_file_size);
	}

	if(rc < 0) {
		this->content_length = this->cl;
	}

	return rc;
}

int64_t FlvTag::write_out(TSIOBuffer buffer) {
	int64_t head_avail;

	head_avail = TSIOBufferReaderAvail(head_reader);

	if (head_avail > 0) {
		TSIOBufferCopy(buffer, head_reader, head_avail, 0);
		TSIOBufferReaderConsume(head_reader, head_avail);
	}

	return head_avail;
}

int FlvTag::process_drm_header() //parse pcf header signature, version, videoid tag, userid tag, reserved tag
{

	int64_t avail;
	size_t drm_header_size = get_drm_header_size();
	drm_header header;
	int result;

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < (int64_t)drm_header_size)
		return 0;

	TSIOBufferCopy(head_buffer, tag_reader, drm_header_size, 0);
	result = flv_read_drm_header(tag_reader, &header);

	tag_pos += drm_header_size;

	if(result != 0) {
		return -1;
	}

	version = swap_uint32(header.version);
	if (version <= 0)
		return -1;

	videoid_size = swap_uint32(header.videoid_size);
	if (videoid_size <= 0)
		return -1;


	this->current_handler = &FlvTag::process_drm_header_videoid;
	return process_drm_header_videoid();
}

int FlvTag::process_drm_header_videoid() {
	int64_t avail;
	size_t userid_size_length = sizeof(uint32_t);
	int64_t read_size = videoid_size + userid_size_length;

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < read_size)
		return 0;

	videoid = (u_char *) TSmalloc(sizeof(u_char) * (videoid_size));
	IOBufferReaderCopy(tag_reader, videoid, videoid_size);
	TSIOBufferCopy(head_buffer, tag_reader, videoid_size, 0);
	TSIOBufferReaderConsume(tag_reader, videoid_size);

	IOBufferReaderCopy(tag_reader, &userid_size, userid_size_length);
	userid_size = swap_uint32(userid_size);
	TSIOBufferCopy(head_buffer, tag_reader, userid_size_length, 0);
	TSIOBufferReaderConsume(tag_reader, userid_size_length);

	tag_pos += read_size;

	if (userid_size <= 0)
		return -1;

//	TSDebug(PLUGIN_NAME,"process_header_videoid videoid=%.*s, userid_size=%d, tag_pos=%ld",
//			videoid_size, videoid, userid_size, tag_pos);

	this->current_handler = &FlvTag::process_drm_header_userid;
	return process_drm_header_userid();
}

int FlvTag::process_drm_header_userid() {
	int64_t avail;
	size_t reserved_size_length = sizeof(uint32_t);
	int64_t read_size = userid_size + reserved_size_length;

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < read_size)
		return 0;

	userid = (u_char *) TSmalloc(sizeof(u_char) * (userid_size));
	IOBufferReaderCopy(tag_reader, userid, userid_size);
	TSIOBufferCopy(head_buffer, tag_reader, userid_size, 0);
	TSIOBufferReaderConsume(tag_reader, userid_size);

	IOBufferReaderCopy(tag_reader, &reserved_size, reserved_size_length);
	reserved_size = swap_uint32(reserved_size);
	TSIOBufferCopy(head_buffer, tag_reader, reserved_size_length, 0);
	TSIOBufferReaderConsume(tag_reader, reserved_size_length);

	tag_pos += read_size;

	drm_head_length = tag_pos;

//	TSDebug(PLUGIN_NAME,"process_header_userid userid=%.*s, reserved_size=%d, tag_pos=%ld",userid_size, userid, reserved_size, tag_pos);

	if (reserved_size <= 0) {
		this->current_handler = &FlvTag::process_decrypt_flv_body;
		return process_decrypt_flv_body();

	} else {
		this->current_handler = &FlvTag::process_drm_header_reserved;
		return process_drm_header_reserved();
	}
}

int FlvTag::process_drm_header_reserved() {
	int64_t avail;

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < reserved_size)
		return 0;

	reserved = (u_char *) TSmalloc(sizeof(u_char) * (reserved_size));
	IOBufferReaderCopy(tag_reader, reserved, reserved_size);
	TSIOBufferCopy(head_buffer, tag_reader, reserved_size, 0);
	TSIOBufferReaderConsume(tag_reader, reserved_size);

	tag_pos += reserved_size;

	drm_head_length = tag_pos;


	this->current_handler = &FlvTag::process_decrypt_flv_body;
	return process_decrypt_flv_body();
}

int FlvTag::process_decrypt_flv_body() {

	int64_t avail;
	u_char *des_buf;
	uint32_t i;
	size_t need_length;
	if(version == VIDEO_VERSION_1) {
		flv_need_des_length = FLV_1_NEED_DES_LENGTH;
		flv_des_length = FLV_1_DES_LENGTH;
		flv_des_section_count = FLV_1_DES_SECTION_COUNT;
		need_length = FLV_1_DES_LENGTH * FLV_1_DES_SECTION_COUNT;
	} else if(version == VIDEO_VERSION_3) {
		flv_need_des_length = FLV_3_NEED_DES_LENGTH;
		flv_des_length = FLV_3_DES_LENGTH;
		flv_des_section_count = FLV_3_DES_SECTION_COUNT;
		need_length = FLV_3_DES_LENGTH * FLV_3_DES_SECTION_COUNT;
	} else {
		return -1;
	}
	if(cl <= (drm_head_length + (int64_t)need_length))
		return -1;

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < (int64_t)need_length)
		return 0;

	TSIOBufferCopy(des_buffer, tag_reader, avail, 0);
	TSIOBufferReaderConsume(tag_reader, avail);

	des_buf = (u_char *)TSmalloc(sizeof(u_char) * flv_des_length);
	for(i = 0; i < flv_des_section_count; i++) {
		memset(des_buf, 0, flv_des_length);
		IOBufferReaderCopy(des_reader, des_buf, flv_des_length);
		des_decrypt(tdes_key, des_buf, flv_des_length);
		TSIOBufferWrite(tag_buffer, des_buf, flv_need_des_length);
		TSIOBufferReaderConsume(des_reader, flv_des_length);
	}

	TSfree((char *)des_buf);
	des_buf = NULL;

	avail = TSIOBufferReaderAvail(des_reader);

	if (avail > 0) {
		TSIOBufferCopy(tag_buffer, des_reader, avail, 0);
	}

	tag_pos += (flv_des_length - flv_need_des_length) * flv_des_section_count;

	TSDebug(PLUGIN_NAME, "process_decrypt_body tag_pos=%ld", tag_pos);

	this->current_handler = &FlvTag::process_initial_flv_header;
	return process_initial_flv_header();
}


int FlvTag::process_initial_flv_header() {
	int64_t avail;
	flv_header header;
	int result;
	size_t flv_header_size = get_flv_header_size();
	size_t need_length  = flv_header_size +  sizeof(uint32_be);  // flv_header + first previoustagsize 第一个默认为0
	//header长度4bytes 整个文件头的长度，一般是9（3+1+1+4），当然头部字段也有可能包含其它信息这个时间其长度就不是9了。
	//FLV Body
	//FLV body就是由很多tag组成的，一个tag包括下列信息：
	//      previoustagsize 4bytes 前一个tag的长度，第一个tag就是0

	avail = TSIOBufferReaderAvail(tag_reader);
	if (avail < (int64_t)need_length)
		return 0;

	TSIOBufferCopy(flv_buffer, tag_reader, need_length, 0);
	result = flv_read_flv_header(tag_reader, &header);
	TSIOBufferReaderConsume(tag_reader, sizeof(uint32_be));

	tag_pos += need_length;

	if(result != 0) {
		return -1;
	}


	this->current_handler = &FlvTag::process_initial_body;
	return process_initial_body();
}

//解析metadataTag
int FlvTag::process_initial_body() {
	uint64_t avail, sz;
	uint32 body_length;
	size_t flv_tag_length = get_flv_tag_size(); //
	avail = TSIOBufferReaderAvail(tag_reader);

	do {
		flv_tag tag;
		if (avail < flv_tag_length)
			return 0;

		flv_read_flv_tag(tag_reader, &tag);
		body_length = flv_tag_get_body_length(tag);

		sz = flv_tag_length + body_length + sizeof(uint32_be); //tag->(tag header, tag body), tagsize

		if (avail < sz)     // insure the whole tag
			return 0;

		if (tag.type == FLV_TAG_TYPE_VIDEO ) {
			goto end;
		} else if(tag.type  == FLV_TAG_TYPE_META) {
			on_meta_data_size = body_length;
		}

		TSIOBufferCopy(flv_buffer, tag_reader, sz, 0);
		TSIOBufferReaderConsume(tag_reader, sz);

		avail -= sz;
		tag_pos += sz;

	} while (avail > 0);

	return 0;

end:

	avail = TSIOBufferReaderAvail(flv_reader);
	video_body_size += avail; //url?start=datasize (包括flv head + flv 脚本，即所有内容)

	this->current_handler = &FlvTag::process_medial_body;
	return process_medial_body();
}

//丢失视频和音频
int FlvTag::process_medial_body() {
	int64_t avail, sz;
	uint32 body_length, timestamp;
	size_t flv_tag_length = get_flv_tag_size();

	avail = TSIOBufferReaderAvail(tag_reader);

	do {
		flv_tag tag;
		if (avail < (int64_t)flv_tag_length)
			return 0;

		flv_read_flv_tag(tag_reader, &tag);
		body_length = flv_tag_get_body_length(tag);
		sz = flv_tag_length + body_length + sizeof(uint32_be); //tag->(tag header, tag body), tagsize

		if (avail < sz)     // insure the whole tag
			return 0;

		video_body_size += sz;

		if (tag.type == FLV_TAG_TYPE_VIDEO ) {

			timestamp = flv_tag_get_timestamp(tag);

			if ((int64_t)video_body_size <= start) {
				duration_time = timestamp; //ms
				duration_video_size += flv_tag_length + body_length;

			} else {
				TSIOBufferCopy(flv_buffer, tag_reader, avail, 0);
				TSIOBufferReaderConsume(tag_reader, avail);
				tag_pos += avail;
				duration_time = duration_time/1000;

//				TSDebug(PLUGIN_NAME, "process_medial_body success!!! duration_audio_size＝%ld, tag_pos= %ld",duration_audio_size,tag_pos);
				this->current_handler = &FlvTag::process_check_des_body;
				return process_check_des_body();
			}
		} else if(tag.type  == FLV_TAG_TYPE_AUDIO) {
			duration_audio_size += flv_tag_length + body_length;
		}

		TSIOBufferReaderConsume(tag_reader, sz);
		duration_file_size += sz;

		avail -= sz;

		tag_pos += sz;

	} while (avail > 0);

	return 0;
}

int FlvTag::update_flv_meta_data() {
	size_t flv_header_length = get_flv_header_size() + sizeof(uint32_be);//sizeof(flv_header) + sizeof(uint32_be);
	size_t flv_tag_size = get_flv_tag_size();
	TSIOBufferCopy(new_flv_buffer, flv_reader, flv_header_length, 0);
	TSIOBufferReaderConsume(flv_reader, flv_header_length);
	TSDebug(PLUGIN_NAME, "update_flv_meta_data copy flv header + first tag size to new_flv_buffer");

	//parse flv tag
	flv_tag tag;
	uint32 body_length;
	amf_data * name;
	amf_data * data;
	amf_data * on_metadata, *on_metadata_name;
	byte *buf;
	uint32 prev_tag_size;
	size_t on_medata_size;
	name = NULL;
	data = NULL;
	on_metadata = NULL;
	on_metadata_name = NULL;
	on_medata_size = 0;


	flv_read_flv_tag(flv_reader, &tag);
	TSIOBufferReaderConsume(flv_reader, flv_tag_size);

	body_length = flv_tag_get_body_length(tag);


	buf = (byte *)TSmalloc(sizeof(byte) * body_length);
	memset(buf, 0, body_length);
	IOBufferReaderCopy(flv_reader, buf, body_length);
	TSIOBufferReaderConsume(flv_reader, body_length);
	flv_read_metadata(buf, &name, &data,body_length);
	IOBufferReaderCopy(flv_reader, &prev_tag_size, sizeof(uint32_be));
	TSIOBufferReaderConsume(flv_reader, sizeof(uint32_be));
	prev_tag_size = swap_uint32(prev_tag_size);
	/* onMetaData checking */
	if (!strcmp((char*) amf_string_get_bytes(name),"onMetaData")) {
		on_medata_size = amf_data_size(data);
		on_metadata = amf_data_clone(data);
		on_metadata_name = amf_data_clone(name);
		/* check onMetadata type */
		if (amf_data_get_type(on_metadata) != AMF_TYPE_ASSOCIATIVE_ARRAY) {
			TSDebug(PLUGIN_NAME,"invalid onMetaData data type: %u, should be an associative array (8)\n",amf_data_get_type(on_metadata));
			amf_data_free(name);
			amf_data_free(data);
			TSfree(buf);
			goto end;
		}
	}
	amf_data_free(name);
	amf_data_free(data);
	TSfree(buf);

    //parse metadata
    amf_node * n;
	/* more metadata checks */
	for (n = amf_associative_array_first(on_metadata); n != NULL; n =
			amf_associative_array_next(n)) {
		byte * name;
		amf_data * data;
		byte type;
		double data_value;

		name = amf_string_get_bytes(amf_associative_array_get_name(n));
		data = amf_associative_array_get_data(n);
		type = amf_data_get_type(data);

		/* TODO: check UTF-8 strings, in key, and value if string type */
		/* duration (number) */
		if (!strcmp((char*) name, "duration")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_duration;
				file_duration = amf_number_get_value(data);
				if(duration_time > file_duration)
					goto end;
				data_value = int2double(file_duration) - duration_time;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for duration: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* lasttimestamp: (number) */
		if (!strcmp((char*) name, "lasttimestamp")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lasttimestamp;
				file_lasttimestamp = amf_number_get_value(data);
				if(duration_time > file_lasttimestamp)
					goto end;
				data_value = int2double(file_lasttimestamp) - duration_time;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lasttimestamp: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* lastkeyframetimestamp: (number) */
		if (!strcmp((char*) name, "lastkeyframetimestamp")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_lastkeyframetimestamp;
				file_lastkeyframetimestamp = amf_number_get_value(data);
				if(duration_time > file_lastkeyframetimestamp)
					goto end;
				data_value = int2double(file_lastkeyframetimestamp) - duration_time;;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for lastkeyframetimestamp: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* filesize: (number) */
		if (!strcmp((char*) name, "filesize")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_filesize;

				file_filesize = amf_number_get_value(data);
				data_value = int2double(file_filesize) - duration_file_size;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* videosize: (number) */
		if (!strcmp((char*) name, "videosize")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_videosize;
				file_videosize = amf_number_get_value(data);
				data_value = int2double(file_videosize) - duration_video_size;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for videosize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* audiosize: (number) */
		if (!strcmp((char*) name, "audiosize")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_audiosize;
				file_audiosize = amf_number_get_value(data);
				data_value = int2double(file_audiosize) - duration_audio_size;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for audiosize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* datasize: (number) */
		if (!strcmp((char*) name, "datasize")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_datasize;
				file_datasize = amf_number_get_value(data);
				data_value = int2double(file_datasize) - duration_video_size - duration_audio_size;
				amf_number_set_value(data,double2int(data_value));
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for datasize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

		/* keyframes: (object) */
		if (!strcmp((char*) name, "keyframes")) {
			if (type == AMF_TYPE_OBJECT) {
				amf_data * file_times, *file_filepositions;

				file_times = amf_object_get(data, "times");
				file_filepositions = amf_object_get(data, "filepositions");

				/* check sub-arrays' presence */
				if (file_times == NULL) {
					TSDebug(PLUGIN_NAME,"Missing times metadata\n");
				}
				if (file_filepositions == NULL) {
					TSDebug(PLUGIN_NAME,"Missing filepositions metadata\n");
				}

				if (file_times != NULL && file_filepositions != NULL) {
					/* check types */
					uint8 times_type, fp_type;

					times_type = amf_data_get_type(file_times);
					if (times_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"times_type != AMF_TYPE_ARRAY －－ invalid type for times: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(times_type));
					}

					fp_type = amf_data_get_type(file_filepositions);
					if (fp_type != AMF_TYPE_ARRAY) {
						TSDebug(PLUGIN_NAME,"fp_type != AMF_TYPE_ARRAY －－ invalid type for filepositions: expected %s, got %s\n",
								get_amf_type_string(AMF_TYPE_ARRAY),
								get_amf_type_string(fp_type));
					}

					if (times_type == AMF_TYPE_ARRAY && fp_type == AMF_TYPE_ARRAY) {
						number64 last_file_time;
						int have_last_time;
						amf_node * ff_node, *ft_node;

						/* iterate in parallel, report diffs */
						last_file_time = 0;
						have_last_time = 0;

						ft_node = amf_array_first(file_times);
						ff_node = amf_array_first(file_filepositions);

						amf_node * first_t_node, *first_p_node;
						first_t_node = ft_node;
						first_p_node = ff_node;
						number64 f_time,f_position;
						double df_time ,df_position;

						while (ft_node != NULL && ff_node != NULL) {
							f_time =0;
							f_position = 0;
							df_time = 0;
							df_position = 0;
							/* time */
							if (amf_data_get_type(amf_array_get(ft_node)) != AMF_TYPE_NUMBER) {
								TSDebug(PLUGIN_NAME,"!= AMF_TYPE_NUMBER  －－ invalid type for time: expected %s, got %s\n",
										get_amf_type_string(AMF_TYPE_NUMBER),
										get_amf_type_string(type));
							} else {
								f_time = amf_number_get_value(amf_array_get(ft_node));
								df_time = int2double(f_time);
								/* check for duplicate time, can happen in H.264 files */
								if (have_last_time && last_file_time == f_time) {
									TSDebug(PLUGIN_NAME,"Duplicate keyframe time: %lf\n",int2double(f_time));
								}
								have_last_time = 1;
								last_file_time = f_time;
							}

							/* position */
							if (amf_data_get_type(amf_array_get(ff_node)) != AMF_TYPE_NUMBER) {
								TSDebug(PLUGIN_NAME,
										"!= AMF_TYPE_NUMBER invalid type for file position: expected %s, got %s\n",
										get_amf_type_string(AMF_TYPE_NUMBER),
										get_amf_type_string(type));
							} else {
								f_position = amf_number_get_value(amf_array_get(ff_node));
								TSDebug(PLUGIN_NAME,"invalid keyframe file position: expected  got %lf\n",int2double(f_position));
							}
							if(df_time!= 0 && df_time <= duration_time) {
								first_t_node->next = amf_array_next(ft_node);
								first_p_node->next = amf_array_next(ff_node);
								first_t_node->next->prev = first_t_node;
								first_p_node->next->prev = first_p_node;

								amf_array_delete(file_times,ft_node);
								amf_array_delete(file_filepositions,ff_node);
								ft_node = first_t_node->next;
								ff_node = first_p_node->next;

								continue;
							} else if(df_time!= 0){ //修改关键帧 和关键帧位置
								amf_number_set_value(amf_array_get(ft_node), double2int(df_time - duration_time));
								amf_number_set_value(amf_array_get(ff_node), double2int(df_position - duration_file_size));
							}

							/* next entry */
							ft_node = amf_array_next(ft_node);
							ff_node = amf_array_next(ff_node);
						}
					}
				}
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for keyframes: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_BOOLEAN),
						get_amf_type_string(type));
			}
		}//end keyframes
	}// end for


	//on_medata_size - amf_data_size(on_metadata);  //等于丢弃了多少关键帧
	for (n = amf_associative_array_first(on_metadata); n != NULL; n =
			amf_associative_array_next(n)) {
		byte * name;
		amf_data * data;
		byte type;

		name = amf_string_get_bytes(amf_associative_array_get_name(n));
		data = amf_associative_array_get_data(n);
		type = amf_data_get_type(data);
		/* filesize: (number) */
		if (!strcmp((char*) name, "filesize")) {
			if (type == AMF_TYPE_NUMBER) {
				number64 file_filesize;

				file_filesize = amf_number_get_value(data);
				double data_value = int2double(file_filesize) -  (on_medata_size - amf_data_size(on_metadata));  //此处有bug ，因为在删除onmetadata关键帧的时候, 大小会再次变化，还需要再次修改
				TSDebug(PLUGIN_NAME,"filesize should be got %lf %lf\n", data_value,int2double(file_filesize));
				amf_number_set_value(data,double2int(data_value));
				break;
			} else {
				TSDebug(PLUGIN_NAME,"invalid type for filesize: expected %s, got %s\n",
						get_amf_type_string(AMF_TYPE_NUMBER),
						get_amf_type_string(type));
			}
		}

	}

end:
	uint32 meta_data_length = amf_data_size(on_metadata_name) + amf_data_size(on_metadata);
	prev_tag_size = flv_tag_size + meta_data_length;

	tag.body_length = uint32_to_uint24_be(meta_data_length);
	IOBufferReaderCopy(new_flv_reader, &tag, flv_tag_size);

	byte metadata_name_b[amf_data_size(on_metadata_name)];
	amf_data_buffer_write(on_metadata_name,metadata_name_b, amf_data_size(on_metadata_name));
	IOBufferReaderCopy(new_flv_reader, metadata_name_b, amf_data_size(on_metadata_name));

	byte *on_medata_data_b = (byte *)TSmalloc(sizeof(byte) * amf_data_size(on_metadata));
	amf_data_buffer_write(on_metadata,on_medata_data_b, amf_data_size(on_metadata));
	IOBufferReaderCopy(new_flv_reader, on_medata_data_b , amf_data_size(on_metadata));
	/* first "previous tag size" */
	uint32_be size = swap_uint32(prev_tag_size);
	IOBufferReaderCopy(new_flv_reader, &size, sizeof(uint32_be));

	amf_data_free(on_metadata);
	amf_data_free(on_metadata_name);

	return 0;
}

int FlvTag::process_check_des_body() {

	int64_t avail, b_avail;
	uint32_t i = 0;
	u_char *buf;
	size_t need_read_length =  get_flv_header_size()+sizeof(uint32_be) *2 + get_flv_tag_size()+on_meta_data_size;//sizeof(flv_header)+sizeof(uint32_be) *2 +sizeof(flv_tag)+on_meta_data_size;
	size_t need_des_length = flv_need_des_length * flv_des_section_count;


	b_avail = TSIOBufferReaderAvail(flv_reader);
	avail = TSIOBufferReaderAvail(tag_reader);
	if ((avail + b_avail) < ((int64_t)need_des_length < (int64_t)need_read_length ? (int64_t)need_read_length: (int64_t)need_des_length))
		return 0;

	if (avail) {
		TSIOBufferCopy(flv_buffer, tag_reader, avail, 0);
		TSIOBufferReaderConsume(tag_reader, avail);
		tag_pos += avail;
	}

	update_flv_meta_data();

	b_avail = TSIOBufferReaderAvail(flv_reader);
	if(b_avail > 0) {
		TSIOBufferCopy(new_flv_buffer, flv_reader, b_avail, 0);
		TSIOBufferReaderConsume(flv_reader, b_avail);
	}

	if(this->video_type == VIDEO_PCF) {

		buf = (u_char *)TSmalloc(sizeof(u_char) * flv_des_length);

		for(i = 0; i < flv_des_section_count; i++) {
			memset(buf, 0, flv_des_length);
			IOBufferReaderCopy(new_flv_reader, buf, flv_need_des_length);
			TSIOBufferReaderConsume(new_flv_reader, flv_need_des_length);
			des_encrypt(tdes_key, buf, flv_need_des_length);
			TSIOBufferWrite(head_buffer, buf, flv_des_length);
		}

		TSfree((char *)buf);
		buf = NULL;
	}


	avail = TSIOBufferReaderAvail(new_flv_reader);
	if (avail > 0) {
		TSIOBufferCopy(head_buffer, new_flv_reader, avail, 0);
		TSIOBufferReaderConsume(new_flv_reader, avail);
	}
	return 1;
}

static int64_t IOBufferReaderCopy(TSIOBufferReader readerp, void *buf,
		int64_t length) {
	int64_t avail, need, n;
	const char *start;
	TSIOBufferBlock blk;

	n = 0;
	blk = TSIOBufferReaderStart(readerp);

	while (blk) {
		start = TSIOBufferBlockReadStart(blk, readerp, &avail);
		need = length < avail ? length : avail;

		if (need > 0) {
			memcpy((char *) buf + n, start, need);
			length -= need;
			n += need;
		}

		if (length == 0)
			break;

		blk = TSIOBufferBlockNext(blk);
	}

	return n;
}


int FlvTag::flv_read_metadata(byte *stream,amf_data ** name, amf_data ** data, size_t maxbytes) {
    amf_data * d;
//    byte error_code;

    /* read metadata name */
//    d = amf_data_file_read(stream);
    d = amf_data_buffer_read(stream,maxbytes);
    *name = d;
//    error_code = amf_data_get_error_code(d);

    size_t name_length = amf_data_size(d);
    /* if only name can be read, metadata are invalid */
//    data_size = amf_data_size(d);

    /* read metadata contents */
    d = amf_data_buffer_read(stream+name_length ,maxbytes);
    *data = d;
//    error_code = amf_data_get_error_code(d);

//    data_size = amf_data_size(d);

    return 0;
}

/* get string representing given AMF type */
static const char * get_amf_type_string(byte type) {
	switch (type) {
	case AMF_TYPE_NUMBER:
		return "Number";
	case AMF_TYPE_BOOLEAN:
		return "Boolean";
	case AMF_TYPE_STRING:
		return "String";
	case AMF_TYPE_NULL:
		return "Null";
	case AMF_TYPE_UNDEFINED:
		return "Undefined";
		/*case AMF_TYPE_REFERENCE:*/
	case AMF_TYPE_OBJECT:
		return "Object";
	case AMF_TYPE_ASSOCIATIVE_ARRAY:
		return "Associative array";
	case AMF_TYPE_ARRAY:
		return "Array";
	case AMF_TYPE_DATE:
		return "Date";
		/*case AMF_TYPE_SIMPLEOBJECT:*/
	case AMF_TYPE_XML:
		return "XML";
	case AMF_TYPE_CLASS:
		return "Class";
	default:
		return "Unknown type";
	}
}

size_t FlvTag::get_flv_tag_size() {
	flv_tag  tag;
	return (sizeof(tag.type) + sizeof(tag.body_length) +sizeof(tag.timestamp) + sizeof(tag.timestamp_extended) + sizeof(tag.stream_id));
}

int FlvTag::flv_read_flv_tag(TSIOBufferReader readerp, flv_tag * tag) {

	size_t flv_tag_size = get_flv_tag_size();
	byte buf[flv_tag_size];
	IOBufferReaderCopy(readerp, buf, flv_tag_size);

	memcpy(&tag->type,buf,sizeof(tag->type));
	memcpy(&tag->body_length,buf + sizeof(tag->type),sizeof(tag->body_length));
	memcpy(&tag->timestamp,buf +sizeof(tag->type) +sizeof(tag->body_length) ,sizeof(tag->timestamp));
	memcpy(&tag->timestamp_extended,buf + sizeof(tag->type) +sizeof(tag->body_length) +sizeof(tag->timestamp),sizeof(tag->timestamp_extended));
	memcpy(&tag->stream_id,buf + sizeof(tag->type) +sizeof(tag->body_length) +sizeof(tag->timestamp) +sizeof(tag->timestamp_extended),sizeof(tag->stream_id));

    return 0;
}

size_t FlvTag::get_flv_header_size() {
	flv_header  header;
	return (sizeof(header.signature) + sizeof(header.version) +sizeof(header.flags) + sizeof(header.offset));
}

int FlvTag::flv_read_flv_header(TSIOBufferReader readerp, flv_header * header) {

	IOBufferReaderCopy(readerp, &header->signature, sizeof(header->signature));
	TSIOBufferReaderConsume(readerp, sizeof(header->signature));

	IOBufferReaderCopy(readerp, &header->version, sizeof(header->version));
	TSIOBufferReaderConsume(readerp, sizeof(header->version));

	IOBufferReaderCopy(readerp, &header->flags, sizeof(header->flags));
	TSIOBufferReaderConsume(readerp, sizeof(header->flags));

	IOBufferReaderCopy(readerp, &header->offset, sizeof(header->offset));
	TSIOBufferReaderConsume(readerp, sizeof(header->offset));

    if (header->signature[0] != 'F'
    || header->signature[1] != 'L'
    || header->signature[2] != 'V') {
        return -1;
    }

    return 0;
}

size_t FlvTag::get_drm_header_size() {
	drm_header  header;
	return (sizeof(header.signature) + sizeof(header.version) +sizeof(header.videoid_size));
}

int FlvTag::flv_read_drm_header(TSIOBufferReader readerp, drm_header * header) {

	IOBufferReaderCopy(readerp, &header->signature, sizeof(header->signature));
	TSIOBufferReaderConsume(readerp, sizeof(header->signature));

	IOBufferReaderCopy(readerp, &header->version, sizeof(header->version));
	TSIOBufferReaderConsume(readerp, sizeof(header->version));

	IOBufferReaderCopy(readerp, &header->videoid_size, sizeof(header->videoid_size));
	TSIOBufferReaderConsume(readerp, sizeof(header->videoid_size));

    if (header->signature[0] != 'P'
    || header->signature[1] != 'C'
    || header->signature[2] != 'F') {
        return -1;
    }

    return 0;
}
