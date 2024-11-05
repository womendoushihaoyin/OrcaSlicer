function OnInit() {
    console.log("Online Model Initialized");
    sendMessageToParent();

    // 绑定功能函数
    $('#swiper-button-prev').on('click', function() {
        onSwiperPrevClick();
    });

    $('#swiper-button-next').on('click', function() {
        onSwiperNextClick();
    });
}

function sendMessageToParent(){
    const message = 'Hello from child';
    window.parent.postMessage(message, '*');
}

var m_ModelList = null;
function UpdateModelContent( ModelList, isadd = false )
{
    if(ModelList ==  m_ModelList){
        return;
    }else{
        m_ModelList =  ModelList;
    }

    if(!isadd){
        $('.portal-css-1skxrux').first().html('');
    }
    
	let PickTotal=ModelList.length;
	if(PickTotal==0)
	{
        if(isadd){
            $('.portal-css-1skxrux').first().append('<div class="portal-css-1skxrux">已经到底了</div>');
            page_bottom = true;
        }else{
            $('.portal-css-1skxrux').first().html('暂无数据');
        }
		return;
	}

	for(let a=0;a<PickTotal;a++)
	{
		let OnePickModel=ModelList[a];
		
		makeModelCard(OnePickModel);
	}
    
}

function ExNumber( number )
{
	let nNew=number;
	if( number>=1000*1000*1000 )
    {
		nNew=Math.round(number/(1000*1000*1000)*10)/10;
		nNew=nNew+'b';
	}
	else if( number>=1000*1000 )
    {
		nNew=Math.round(number/(1000*1000)*10)/10;
		nNew=nNew+'m';
	}
	if( number>=1000 )
    {
		nNew=Math.round(number/(1000)*10)/10;
		nNew=nNew+'k';
	}	
	
	return nNew;
}

function makeModelCard(OnePickModel)
{
    let ModelID=OnePickModel['design']['id'];
		let ModelName=OnePickModel['design']['title'];
		let ModelCover=OnePickModel['design']['cover']+'?image_process=resize,w_360/format,webp';
		
		let DesignerName=OnePickModel['design']['designCreator']['name'];
		let DesignerAvatar=OnePickModel['design']['designCreator']['avatar']+'?image_process=resize,w_32/format,webp';
		
		let NumZan=OnePickModel['design']['likeCount'];
		let NumDownload=OnePickModel['design']['downloadCount'];
		NumZan=ExNumber(NumZan);
		NumDownload=ExNumber(NumDownload);
		
        // make the model card
        let modelImg_img = $('<img  />');
        modelImg_img.attr('src', ModelCover);
        modelImg_img.attr('alt', ModelName);
        modelImg_img.addClass('gif-image lazy portal-css-1e5ufbu');

        let modelImg_div = $('<div></div>');
        modelImg_div.addClass('portal-css-1f8sh1y');
        modelImg_div.append(modelImg_img);

        let multiBox_div = $('<div></div>');
        multiBox_div.addClass('MuiBox-root portal-css-b4kcmh');
        multiBox_div.append(modelImg_div);

        let div_1 = $('<div></div>');
        div_1.addClass('portal-css-170eg3f');
        div_1.append(multiBox_div);

        let a_label = $('<a></a>');
        a_label.attr('href', 'javascript:void(0);');
        a_label.append(div_1);
        a_label.attr('onclick', 'showModelDetail('+ModelID+');')

        let div_2 = $('<div></div>');
        div_2.addClass('design-cover-wrap portal-css-16eo00i');
        div_2.append(a_label);

        let creator_img = $('<img  />');
        creator_img.attr('src', DesignerAvatar);
        creator_img.attr('alt', DesignerName);
        creator_img.width(40);
        creator_img.height(40);

        let creator_img_div = $('<div></div>');
        creator_img_div.addClass('portal-css-lbe2pk');
        creator_img_div.append(creator_img);

        let div_3 = $('<div></div>');
        div_3.addClass('size-40 portal-css-79elbk');
        div_3.append(creator_img_div);

        let div_4 = $('<div></div>');
        div_4.addClass('portal-css-1xjxise');
        div_4.append(div_3);

        let div_5 = $('<div></div>');
        div_5.addClass('portal-css-s7haq8');
        div_5.append(div_4);

        let a_label_2 = $('<a></a>');
        a_label_2.attr('href', 'javascript:void(0);');
        a_label_2.attr('title', ModelName);
        a_label_2.html(ModelName);

        let h3_1 = $('<h3></h3>');
        h3_1.addClass('translated-text portal-css-1i1ow8v');
        h3_1.append(a_label_2);

        let div_6 = $('<div></div>');
        div_6.addClass('MuiStack-root portal-css-nwm1t5');
        div_6.append(h3_1); //

        let author_name_span = $('<span></span>');
        author_name_span.addClass('author-name');
        author_name_span.append(DesignerName);

        let author_name_div = $('<div></div>');
        author_name_div.addClass('portal-css-1rkgsh0');
        author_name_div.append(author_name_span);

        let div_7 = $('<div></div>');
        div_7.addClass('portal-css-1fyfzor');
        div_7.append(author_name_div);

        let div_8 = $('<div></div>');
        div_8.addClass('portal-css-1xjxise');
        div_8.append(div_7); //

        let svg_zan = $('<svg width="1em" height="1em" viewBox="0 0 20 20" fill="none" xmlns="http://www.w3.org/2000/svg" class="operate_icon"><path fill-rule="evenodd" clip-rule="evenodd" d="M8.56464 1.74618C8.68502 1.47533 8.95361 1.30078 9.25 1.30078C10.9069 1.30078 12.25 2.64393 12.25 4.30079V6.55079H15.9962C16.6546 6.54468 17.2827 6.82735 17.7149 7.32426C18.148 7.82239 18.3406 8.48567 18.2415 9.13834L17.2065 15.8882C17.2065 15.8885 17.2066 15.8879 17.2065 15.8882C17.0382 16.9956 16.0809 17.8111 14.9613 17.8008H6.25C5.83579 17.8008 5.5 17.465 5.5 17.0508V8.80079C5.5 8.69586 5.52202 8.59208 5.56464 8.49619L8.56464 1.74618ZM9.70604 2.87136L7 8.95996V16.3008H14.9735C15.3475 16.305 15.6674 16.0331 15.7235 15.6634L16.7585 8.91325C16.7585 8.91347 16.7585 8.91303 16.7585 8.91325C16.7913 8.6959 16.7272 8.47444 16.583 8.30858C16.4386 8.1425 16.2285 8.04825 16.0085 8.05075L16 8.05084L11.5 8.05079C11.0858 8.05079 10.75 7.71501 10.75 7.30079V4.30079C10.75 3.63135 10.3115 3.06435 9.70604 2.87136Z" fill="currentColor"></path><path fill-rule="evenodd" clip-rule="evenodd" d="M4.25351 7.96705H6.25C6.66421 7.96705 7 8.30284 7 8.71705V17.0501C7 17.4643 6.66421 17.8001 6.25 17.8001H4.25358C2.9977 17.8187 1.92523 16.8961 1.75677 15.6506C1.75226 15.6173 1.75 15.5837 1.75 15.5501V10.3001C1.75 10.2665 1.75226 10.2329 1.75677 10.1996C1.91847 9.00416 2.94943 7.94796 4.25351 7.96705ZM3.25 10.3571V15.4925C3.33499 15.9645 3.75053 16.3088 4.23424 16.3002L4.2475 16.3L5.5 16.3001V9.46705H4.23423C3.79658 9.45931 3.34191 9.83561 3.25 10.3571Z" fill="currentColor"></path></svg>');
        let num_zan_span = $('<span></span>');
        num_zan_span.html(NumZan);

        let div_9 = $('<div></div>');
        div_9.addClass("operate portal-css-fdzhoi");
        div_9.append(svg_zan);
        div_9.append(num_zan_span);

        let svg_download = $('<svg width="1em" height="1em" viewBox="0 0 16 16" fill="none" style="cursor: default;"><path fill-rule="evenodd" clip-rule="evenodd" d="M13.25 7.0249C13.5814 7.0249 13.85 7.29353 13.85 7.6249V11.8749C13.85 12.7586 13.1337 13.4749 12.25 13.4749H3.75002C2.86637 13.4749 2.15002 12.7586 2.15002 11.8749V7.62732C2.15002 7.29595 2.41865 7.02732 2.75002 7.02732C3.0814 7.02732 3.35002 7.29595 3.35002 7.62732V11.8749C3.35002 12.0958 3.52911 12.2749 3.75002 12.2749H12.25C12.4709 12.2749 12.65 12.0958 12.65 11.8749V7.6249C12.65 7.29353 12.9187 7.0249 13.25 7.0249Z" fill="currentColor"></path><path fill-rule="evenodd" clip-rule="evenodd" d="M4.95076 6.90865C5.18507 6.67433 5.56497 6.67433 5.79929 6.90865L8.00002 9.10938L10.2008 6.90865C10.4351 6.67433 10.815 6.67433 11.0493 6.90865C11.2836 7.14296 11.2836 7.52286 11.0493 7.75717L8.42429 10.3822C8.18997 10.6165 7.81008 10.6165 7.57576 10.3822L4.95076 7.75717C4.71645 7.52286 4.71645 7.14296 4.95076 6.90865Z" fill="currentColor"></path><path fill-rule="evenodd" clip-rule="evenodd" d="M7.99758 1.8999C8.32895 1.8999 8.59758 2.16853 8.59758 2.4999V9.95824C8.59758 10.2896 8.32895 10.5582 7.99758 10.5582C7.66621 10.5582 7.39758 10.2896 7.39758 9.95824V2.4999C7.39758 2.16853 7.66621 1.8999 7.99758 1.8999Z" fill="currentColor"></path></svg>');
        let num_download_span = $('<span></span>');
        num_download_span.html(NumDownload);

        let div_10 = $('<div></div>');
        div_10.addClass("portal-css-fdzhoi");
        div_10.append(svg_download);
        div_10.append(num_download_span);

        let operate_div = $('<div></div>');
        operate_div.addClass("portal-css-dtg2nl");
        operate_div.append(div_9);
        operate_div.append(div_10);

        let creator_div = $('<div></div>');
        creator_div.addClass("MuiStack-root portal-css-2e5rnz");
        creator_div.append(div_8);
        creator_div.append(operate_div);

        let div_11 = $('<div></div>');
        div_11.addClass("MuiStack-root portal-css-t4waze");
        div_11.append(div_6);
        div_11.append(creator_div);

        let div_12 = $('<div></div>');
        div_12.addClass("design-bottom-row portal-css-1cjn62c");
        div_12.append(div_5);
        div_12.append(div_11);

        let design_div = $('<div></div>');
        design_div.addClass("js-design-card portal-css-sh68va");
        design_div.append(div_2);
        design_div.append(div_12);

        let div_container = $('<div></div>');
        div_container.attr('id', 'model' + ModelID);
        div_container.addClass("portal-css-fhtt0u");
        div_container.append(design_div);




        $('.portal-css-1skxrux').first().append(div_container);
}

function scrollToTarget(modelID){
    const element = document.getElementById('model' + modelID);
    element.scrollIntoView({behavior: "smooth"});
}

function showmodelDetail(data){
    $('#detail-card').css('display', 'block');

    $('#creator-avator').attr('src', data["creatorAvator"] + '?image_process=resize,w_360/format,webp');
    $('#model-name').attr('aria-label', data["name"]);
    $('#model-name').html(data["name"]);

    $('#creator-name').html(data["creator"]);

    $('#model-description').html(data["content"]);

    
    $('#license-pic').attr('src', data["license"][1] + '?image_process=resize,w_120/format,webp');

    $('#download-count').html(846);
    $('#print-count').html(1000);
    $('#publish-time').html('发布于 2024-10-31 18:05');
    $('#zan-count').html(225);
    $('#collection-count').html(25);

    total_pic_count = data["banners"].length;

    updateModelPics(data["banners"]);
    updateModelPicSelector(data["banners"]);
}

function updateModelPics(pics){
    let model_swipper = $('#model-swipper');
    model_swipper.html(''); // 清空原有图片

    // 还原滑动
    $('#model-swipper').attr('style', '0px, 0px, 0px); transition: all 0.3s ease-in-out;');
    current_index = 0;

    if(pics.length <= 0) return;

    let baseWidth = model_swipper.width() / pics.length;

    for(let i  = 0; i < pics.length; i++){
        let div = $('<div></div>');

        div.addClass('swiper-slide');
        if (i == 1) {
            div.addClass('swiper-slide-active');
        } else if (i == 2) {
            div.addClass('swiper-slide-next');
        }
        div.addClass('portal-css-135epe0');

        //div.attr('style', 'margin-right:50px;');
        //div.attr('style', 'width: 305px; margin-right:50px;');

        // div.attr('style', `width: ${baseWidth}px; margin-right:50px;`);

        let img = $('<img />'); // 应该使用img变量而不是modelImg_img
        img.attr('src', pics[i] + '?image_process=resize,w_305/format,webp');
        img.attr('alt', '');
        img.addClass('portal-css-1dn0z63 swiper-lazy');
        img.attr('loading', 'lazy');

        div.append(img); // 将img插入到div中

        model_swipper.append(div); // 将div插入到model_swipper中
        
    }
}


var current_index  = 0; // 当前显示的主图片
var swiper_left_index = 0; // 当前最左侧的轮播图片
var container_pic_count = 3; // 图片显示的数量
var total_pic_count = 0; // 图片总数量

function updateModelPicSelector(pics){
    let model_pics_selector = $('#model-pics-selector');
    model_pics_selector.html(''); // 清空原有图片

    // 还原滑动
    $('#model-pics-selector').attr('style', '0px, 0px, 0px); transition: all 0.3s ease-in-out;');
    swiper_left_index = 0;

    $('#swiper-button-prev').css('display', 'none');
    if(pics.length <= container_pic_count){
        $('#swiper-button-next').css('display', 'none');
    }else{
        $('#swiper-button-next').css('display', 'block');
    }

    if(pics.length <= 0) return;

    let baseWidth = model_pics_selector.width() / container_pic_count - 10;

    for(let i  = 0; i < pics.length; i++){
        let div = $('<div></div>');

        div.addClass('swiper-slide');

        if(i <= 3){
            div.addClass('swiper-slide-visible');

            if(i == 0){
                div.addClass('swiper-slide-thumb-active');
                div.addClass('swiper-slide-active');
            }else if(i == 1){
                div.addClass('swiper-slide-next');
            }
        }
        div.attr('style', `width: ${baseWidth}px; margin-right: 10px;`);

        div_1 = $('<div></div>');
        div_1.addClass('MuiBox-root');
        div_1.addClass('portal-css-1bagh5i');
        if(i == 0){
            div_1.addClass('active');
        }
        div_1.on('click', function(){
            modelPicSelect(i);
        });

        div_2 = $('<div></div>');
        div_2.addClass('MuiBox-root');
        div_2.addClass('portal-css-1kci8bl');

        div_3 = $('<div></div>');
        div_3.addClass('portal-css-1f8sh1y');

        let img = $('<img />');
        img.addClass('lazy');
        img.addClass('portal-css-1e5ufbu');
        img.attr('src', pics[i] + '?image_process=resize,w_100/format,webp');
        img.attr('alt', '');
        img.attr('style', 'border-radius: 5px;');

        div_3.append(img);
        div_2.append(div_3);
        div_1.append(div_2);
        div.append(div_1);
        model_pics_selector.append(div);
    }
}

function showModelDetail(ModelID){
    var postMsg = {};
	postMsg.cmd = "if_show_model_detail";
	postMsg.data = ModelID;
    window.parent.postMessage(postMsg, '*');
}

function modelPicSelect(index){
    if(index == current_index){
        return;
    }

    baseWidth = $($('#model-swipper')[0].children[index]).width();

    $('#model-pics-selector')[0].children[current_index].classList.remove('swiper-slide-thumb-active');
    $('#model-pics-selector')[0].children[current_index].children[0].classList.remove('active');
    $('#model-pics-selector')[0].children[current_index].classList.remove('swiper-slide-active');
    
    $('#model-pics-selector')[0].children[index].classList.add('swiper-slide-thumb-active');
    $('#model-pics-selector')[0].children[index].children[0].classList.add('active');
    $('#model-pics-selector')[0].children[index].classList.add('swiper-slide-active');

    if(current_index - 1 >= 0){
        $('#model-pics-selector')[0].children[current_index - 1].classList.remove('swiper-slide-prev');
    }
    if(current_index + 1 < $('#model-pics-selector')[0].children.length){
        $('#model-pics-selector')[0].children[current_index + 1].classList.remove('swiper-slide-next');
    }
    if(index - 1 >= 0){
        $('#model-pics-selector')[0].children[index - 1].classList.add('swiper-slide-prev');
    }
    if(index + 1 < $('#model-pics-selector')[0].children.length){
        $('#model-pics-selector')[0].children[index + 1].classList.add('swiper-slide-next');
    }
    current_index = index;

    $('#model-swipper').attr('style', 'transform: translate3d(-' +  (index * baseWidth) + 'px, 0px, 0px); transition: all 0.3s ease-in-out;');

}

function onSwiperPrevClick(){
    if(swiper_left_index >= 1){
        swiper_left_index--;
    }

    let baseWidth = $($('#model-pics-selector')[0].children[0]).width();

    $('#model-pics-selector').attr('style', 'transform: translate3d(-' +  (swiper_left_index * baseWidth) + 'px, 0px, 0px); transition: all 0.3s ease-in-out;');

    if(total_pic_count - swiper_left_index > 0){
        $('#swiper-button-next').css('display', 'block');
    }else{
        $('#swiper-button-next').css('display', 'none');
    }

    if(swiper_left_index == 0){
        $('#swiper-button-prev').css('display', 'none');
    }else
    {
        $('#swiper-button-prev').css('display', 'block');
    }
}

function onSwiperNextClick(){
    if(swiper_left_index + container_pic_count < total_pic_count){
        swiper_left_index++;
    }

    let baseWidth = $($('#model-pics-selector')[0].children[0]).width();
    $('#model-pics-selector').attr('style', 'transform: translate3d(-' +  (swiper_left_index * baseWidth) + 'px, 0px, 0px); transition: all 0.3s ease-in-out;');

    if(total_pic_count - swiper_left_index > container_pic_count){
        $('#swiper-button-next').css('display', 'block');
    }else{
        $('#swiper-button-next').css('display', 'none');
    }

    if(swiper_left_index == 0){
        $('#swiper-button-prev').css('display', 'none');
    }else
    {
        $('#swiper-button-prev').css('display', 'block');
    }
}

var lastSearchInput = "";
function searchInputChange(event){
    if(event.keyCode === 13 || event.key === 'Enter'){
        var input = event.target || event.srcElement;

        if(input.value == lastSearchInput){
            return;
        }
        model_page= 0;
        page_bottom = false;
        lastSearchInput = input.value;
        var postMsg = {};
        postMsg.cmd = "if_search_model";
        postMsg.data = input.value;
        postMsg.page = model_search_page;
        window.parent.postMessage(postMsg, '*');
    }
    
}

var model_page = 1;
var model_search_page = 1;
var page_bottom = false;

function handleScrollToBottom(){
    if(page_bottom){
        return;
    }
    var postMsg = {};
    postMsg.cmd = "if_add_request";
    if(lastSearchInput == ""){
        model_page++;
        postMsg.page = model_page;
        postMsg.data = "";
    }else{
        model_search_page++;
        postMsg.page = model_search_page;
        postMsg.data = lastSearchInput;
    }
    window.parent.postMessage(postMsg, '*');
}

document.addEventListener("click", function(event){
    var cardArea = $("#card-area")[0];
    if(!cardArea.contains(event.target)){
        $('#detail-card').css('display', 'none');
    }
});

window.addEventListener('scroll', function() {
    if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight) {
        handleScrollToBottom();
    }
});

window.addEventListener('message', function (event) {
    if (event.data.cmd  == 'if_hot_model_advise_get'){
        UpdateModelContent(event.data.data);
    }else if(event.data.cmd  == 'model_target'){
        scrollToTarget(event.data.data);
    }else if(event.data.cmd  == "modelmall_model_open"){
        showmodelDetail(event.data.data);
    }else if(event.data.cmd  == "model_logout"){
        $('#model-user-avatar').attr('src', './img/avatar.webp');
    }else if(event.data.cmd  == "model_login"){
        $('#model-user-avatar').attr('src', event.data.data);
    }else if(event.data.cmd  == "model_page_update"){
        UpdateModelContent(event.data.data, event.data.isadd);
    }
});

